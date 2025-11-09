# Hospital Lighting Control System

Three PlatformIO projects plus a shared contracts header implement a Redis-driven demo for hospital room lighting. Every room has a local control stack (UNO panel + ESP8266 sender) that publishes desired LED states into Redis, and an ESP8266 receiver that applies the latest snapshot plus future stream updates.

## Repository Layout

- `include/config.example.h` - copy to `include/config.h` and edit Wi-Fi/Redis settings.
- `contracts/` - header-only library with Redis key helpers and JSON codecs.
- `esp-sender/` - ESP8266 D1 mini that ingests UNO JSON over UART and writes Redis keys/streams.
- `esp-receiver/` - ESP8266 D1 mini that provisions a room id, applies LED PWM, heartbeats, and acks.
- `arduino-panel/` - UNO R4 Minima potentiometer/button panel that emits newline JSON frames.
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
   pio run -e uno_panel -t upload
   pio run -e esp_sender -t upload
   pio run -e esp_receiver -t upload
   ```
   Open a serial monitor at `115200` when needed (e.g. `pio device monitor -e esp_receiver -b 115200`). You can still run inside a subdirectory with `-d` if you prefer that workflow.

## Wiring Overview

- **UNO panel + ESP sender**
  - UNO `Serial1 TX (D1)` -> level shifter -> ESP8266 `RX0`.
  - UNO `Serial1 RX (D0)` <- level shifter <- ESP8266 `TX0`.
  - Common ground. Provide 3.3V-friendly signals; never drive the ESP pins directly from 5V.
- **Receiver LED output**
  - ESP8266 `D4` drives LED PWM. For anything beyond a small indicator LED, use a MOSFET/transistor and a dedicated LED supply rail (include a flyback diode if inductive).
- **Networking**
  - Both ESP8266 boards operate in STA mode on the same 2.4GHz SSID. Keep them on an isolated lab VLAN when possible.

## Firmware Behavior

- **UNO panel**
  - Reads the potentiometer on `A0` (mapped to 0-100%) and button on `D2` (toggle on/off).
  - Emits newline-terminated JSON such as `{"mode":"on","brightness":72}` over `Serial1` at `115200`.
  - Rate limits by only sending when the button toggles, brightness changes by about 2%, or 1s elapses.
- **ESP sender**
  - Waits for a `ROOM:<id>` line (forwarded from the receiver) before relaying panel frames.
  - Seeds the local `ver` counter from `room:{id}:desired`, injects/increments `ver` if absent, mirrors `room` inside each payload, `SET`s the snapshot, `XADD`s `cmd:room:{id}` and applies `XTRIM MAXLEN ~ 200`.
  - Flushes stalled or overflowing UART frames after 50 ms to avoid partial publishes.
- **ESP receiver**
  - Connects to Wi-Fi/Redis, runs the provisioning Lua (`device:{mac}:room`), prints `ROOM:<id>` over UART, applies the latest desired snapshot, and starts `XREAD BLOCK 1000 STREAMS cmd:room:{id} $`.
  - Drives LED PWM, records `room:{id}:reported`, emits `state:room:{id}` acks, trims streams, and sends `room:{id}:online` heartbeats every 3 s (TTL 10 s). Re-provisions after reconnect and ignores stale `ver`.

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
