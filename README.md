# Hospital Lighting Control System

Two PlatformIO projects plus a shared contracts header implement a Redis-driven demo for hospital room lighting. Each room has an ESP8266 sender that synchronizes time, pulls schedule/configuration data from Redis, computes the desired LED output, and publishes it, while an ESP8266 receiver applies the latest snapshot plus future stream updates.

## Repository Layout

- `include/config.example.h` - copy to `include/config.h` and edit Wi-Fi/Redis settings.
- `contracts/` - header-only library with Redis key helpers and JSON codecs.
- `esp-sender/` - ESP8266 D1 mini scheduler that pulls Redis config, computes desired states, and writes Redis keys/streams.
- `esp-receiver/` - ESP8266 D1 mini that provisions a room id, applies LED PWM, heartbeats, and acks.
- `docker/docker-compose.yml` - Redis 7 with AOF enabled for local testing.

## Quick Start

1. `cp include/config.example.h include/config.h` and adjust:
   - `WIFI_SSID`, `WIFI_PASS`, and optional `WIFI_HOSTNAME`.
   - `REDIS_HOST`, `REDIS_PORT`, `REDIS_PASSWORD`.
2. Bring up Redis:
   ```bash
   cd docker
   docker compose up -d
   ```
   Redis stores data under the named `redis-data` volume with append-only persistence enabled.
3. Build/flash each PlatformIO environment from the repo root (PlatformIO CLI):
```bash
pio run -e esp_sender -t upload
pio run -e esp_receiver -t upload
```
   Open a serial monitor at `115200` when needed (e.g. `pio device monitor -e esp_receiver -b 115200`). You can still run inside a subdirectory with `-d` if you prefer that workflow.

## Wiring Overview

- **ESP sender**
  - Power the D1 mini (or similar ESP8266) from USB or a regulated 3.3 V rail and keep the USB/UART console accessible at `SENDER_CONSOLE_BAUD` so you can answer the `ROOM?` prompt (or just set `ROOM_ID_OVERRIDE`).
  - No external Arduino panel is required; the sender calculates brightness locally from the Redis schedule and current time.
- **Receiver LED output**
  - ESP8266 `D4` drives LED PWM. For anything beyond a small indicator LED, use a MOSFET/transistor and a dedicated LED supply rail (include a flyback diode if inductive).
- **Networking**
  - Both ESP8266 boards operate in STA mode on the same 2.4GHz SSID. Keep them on an isolated lab VLAN when possible.

## Firmware Behavior

- **ESP sender (scheduler)**
  - Prompts for `ROOM:<id>` over USB (or applies `ROOM_ID_OVERRIDE`), synchronizes time via SNTP (`TZ_OFFSET_SECONDS` / `DST_OFFSET_SECONDS`), and polls `room:{id}:cfg` every `SCHEDULE_REFRESH_MS`.
  - Builds the desired LED state from the wake/night schedule plus the configured baseline brightness and only publishes when the computed state changes. Publishes update snapshots to `room:{id}:desired` plus `cmd:room:{id}` with `XTRIM ~ 200`.
  - Falls back to the defaults from `include/config.h` whenever no config is available or JSON is invalid, so the room still has a predictable sunrise/night cycle.
- **ESP receiver**
  - Connects to Wi-Fi/Redis, runs the provisioning Lua (`device:{mac}:room`), prints `ROOM:<id>` over UART, applies the latest desired snapshot, and starts `XREAD BLOCK 1000 STREAMS cmd:room:{id} $`.
  - Drives LED PWM, records `room:{id}:reported`, emits `state:room:{id}` acks, trims streams, and sends `room:{id}:online` heartbeats every 3 s (TTL 10 s). Re-provisions after reconnect and ignores stale `ver`.

## Room Configuration

Room-level settings live at `room:{id}:cfg` as JSON. The sender merges the payload with the defaults from `include/config.h`, so you only need to set what changes per room. Supported fields:

- `baseline.brightness` – percent (0-100) used outside any wake/night window.
- `wake`: `{ "enabled": true, "hour": 7, "minute": 0, "duration_min": 20, "brightness": 100 }` ramps up from the baseline to `brightness` across `duration_min`.
- `night`: `{ "enabled": true, "hour": 22, "minute": 0, "brightness": 5 }` forces a low level from the configured time through the next wake window.
- Optional `version` value is logged for traceability.

Example:

```json
{
  "version": 3,
  "baseline": { "brightness": 0 },
  "wake": { "enabled": true, "hour": 7, "minute": 0, "duration_min": 30, "brightness": 95 },
  "night": { "enabled": true, "hour": 22, "minute": 0, "brightness": 8 }
}
```

## Redis Testing Snippets

Run from any host with `redis-cli`:

```bash
# Inspect the desired snapshot for room 101
redis-cli GET room:101:desired

# Watch receiver acknowledgements
redis-cli XREAD BLOCK 0 STREAMS state:room:101 $

# Check heartbeats (key expires ~10 s after receiver loss)
redis-cli EXISTS room:101:online
```

## Operational Notes

- Streams are trimmed to `~200` entries to bound memory as you scale to ~20 rooms.
- Dynamic provisioning guarantees room ids >= `PROVISIONING_BASE_ID` (default 100) and seeds `room:{id}:desired` if missing.
- The repo never hard-codes room ids; everything flows from the provisioning Lua script.
- If Redis drops, both ESPs fall back to their last applied state and automatically resync once connectivity returns.

For the detailed architecture requirements and acceptance criteria, see `docs/planning.md`.
