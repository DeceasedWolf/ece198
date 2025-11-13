# Hospital Lighting Control System

This repo implements a Redis-backed hospital room lighting demo that pairs two ESP8266 firmware targets with a dependency-free Node.js web UI. The ESP "sender" board acts as the room scheduler and manual override panel, the ESP "receiver" board drives the light output, and the website lets staff adjust quiet hours or trigger overrides without touching hardware.

## Repository Layout

- `include/config.example.h` – copy to `include/config.h` and edit Wi-Fi, Redis, and hardware pin/polarity settings.
- `contracts/` – header-only helper library that defines Redis key conventions, stream names, and `Desired` JSON codecs shared by both PlatformIO projects.
- `esp-sender/` – PlatformIO project for the ESP8266 room scheduler + manual override panel (console prompt, rotary pot, button, optional SSD1306 display, and status LED).
- `esp-receiver/` – PlatformIO project for the ESP8266 LED driver that provisions a room id, tracks command streams, drives PWM (single channel or RGB mix), and reports telemetry.
- `website/` – minimal Node 18 HTTP/RESP server that renders the per-room UI and exposes matching API endpoints.
- `docker-compose.yml` – Redis 7 container with append-only persistence for local development.
- `docs/` – planning notes and acceptance criteria (`docs/planning.md`).

## Quick Start

1. **Configure firmware**
   - `cp include/config.example.h include/config.h`
   - Adjust Wi-Fi (`WIFI_SSID`, `WIFI_PASS`, optional `WIFI_HOSTNAME`) and Redis settings (`REDIS_HOST`, `REDIS_PORT`, `REDIS_PASSWORD`).
   - Set GPIO selections for the receiver LED channels/driver and the manual-override hardware if they differ from the defaults.
2. **Start Redis locally**
   ```bash
   docker compose up -d
   ```
   The stack listens on `localhost:6379` and persists to the named `redis-data` volume.
3. **Build and flash the firmware (PlatformIO CLI)**
   ```bash
   pio run -e esp_sender -t upload
   pio run -e esp_receiver -t upload
   ```
   - Use `pio device monitor -e esp_sender -b 115200` (or `esp_receiver`) to watch logs.
   - The sender prints `ROOM?` until you type `ROOM:<id>` over USB (or set `ROOM_ID_OVERRIDE` in `config.h`).
4. **Run the website**
   ```bash
   cd website
   npm start
   ```
   - Requires Node.js 18+ (uses the built-in `net` module for RESP; there are no npm dependencies).
   - Navigate to `http://localhost:3000/` and open a room such as `/room/101`. The server can also run on another host—set `PORT`/`WEB_PORT`, `REDIS_HOST`, `REDIS_PORT`, and `REDIS_PASSWORD` as needed.

## Firmware Behavior

### ESP sender (scheduler + control panel)

- Prompts for a room id via serial (`ROOM?`) or applies the compile-time `ROOM_ID_OVERRIDE`. Once a room is known it seeds the current desired snapshot (`room:{id}:desired`, falling back to `room:{id}:reported`) to keep version counters monotonic.
- Synchronizes time with SNTP (`TZ_OFFSET_SECONDS` / `DST_OFFSET_SECONDS` + `NTP_SERVER_*`) and polls `room:{id}:cfg` every `SCHEDULE_REFRESH_MS`. Missing or invalid JSON reverts to the defaults in `config.h`.
- Computes the desired brightness from the baseline, programmable sunrise ramp (`wake` window), and quiet-hours dim path (`night` window plus `QUIET_HOURS_DIM_MINUTES`). Only publishes when the calculated state changes or a manual override is active.
- Publishes desired states to `room:{id}:desired` and `cmd:room:{id}` (trimmed to `~200` entries) and mirrors the last packet locally so drops/reconnects continue smoothly.
- Implements the manual override hardware: a debounced button toggles override mode, while the analog potentiometer selects brightness (0–100%). Each change is written to `room:{id}:override` (`enabled`, `ver`, `updated_at`, `source=device`) so the website stays in sync.
- Polls `room:{id}:override` to honor remote toggles from the web UI, pushes overrides immediately to the desired snapshot, and clears the override once disabled.
- Optional UX features: an SSD1306 display (if `SENDER_DISPLAY_ENABLED`) shows current time + quiet-hour window, and a status LED blinks while Redis is healthy (solid when only Wi-Fi is linked).

### ESP receiver (actuator)

- Connects to Wi-Fi STA mode, authenticates to Redis, and executes the provisioning Lua script that maps the device MAC to a room id (`device:{mac}:room`, `rooms:next_id`). If the room did not exist, the script seeds `room:{id}:desired`.
- Prints `ROOM:<id>` on the UART every `ROOM_ANNOUNCE_INTERVAL_MS` so you can log deployments or feed the sender prompt manually.
- Loads the latest desired snapshot, applies PWM to the configured LED channel(s), and records the applied state to both `room:{id}:reported` (overwrite) and `state:room:{id}` (stream trimmed to `~200` entries). Optional RGB wiring mixes duty cycles per channel with independent polarity and mix percentages.
- Subscribes to `cmd:room:{id}` with `XREAD BLOCK 1000` and applies every streamed payload whose `ver` is newer than the last applied version.
- Sends `room:{id}:online` heartbeats (TTL `contracts::kHeartbeatTtlSec`) and exposes LED/Wi-Fi/Redis health via the status LED (off = disconnected, solid = Wi-Fi only, blink = Wi-Fi + Redis).

## Room Configuration (`room:{id}:cfg`)

The sender merges per-room JSON with the defaults in `config.h`, so you only need the fields that differ:

- `baseline.brightness` – default percent (0–100) outside any special window.
- `wake` – `{ "enabled": true, "hour": 7, "minute": 0, "duration_min": 20, "brightness": 100 }`. When enabled, the sender ramps from the baseline to `brightness` in `duration_min` minutes that end at the wake time.
- `night` – `{ "enabled": true, "hour": 22, "minute": 0, "brightness": 5 }`. Once night starts the output is forced to `night.brightness` until the next wake window. The lead-in ramp length is governed by `QUIET_HOURS_DIM_MINUTES`.
- `version` – optional integer tracked in logs and surfaced via the web API.

Example payload:

```json
{
  "version": 3,
  "baseline": { "brightness": 10 },
  "wake": { "enabled": true, "hour": 6, "minute": 30, "duration_min": 30, "brightness": 95 },
  "night": { "enabled": true, "hour": 21, "minute": 0, "brightness": 8 }
}
```

## Manual Override and Website Controls

- Override state lives at `room:{id}:override` and has the shape:
  ```json
  { "enabled": true, "ver": 12, "updated_at": 1713309962000, "source": "website" }
  ```
- When override is enabled (either by the hardware button or the website) the sender switches to the potentiometer-selected brightness and streams that desired state continuously. Disabling override hands control back to the quiet-hours schedule.
- The website exposes:
  - Quiet-hours form (`POST /room/{id}/quiet-hours`) that rewrites `room:{id}:cfg` with the supplied wake/sleep pair (night brightness is forced to 0 for patient comfort).
  - Manual override toggle (`POST /room/{id}/override`) that updates the override key with `source=website`.
  - Instant brightness quick actions (`POST /room/{id}/brightness`) that write `room:{id}:desired` and append `cmd:room:{id}` packets for full-on or lights-out testing.
- `GET /api/rooms/{id}` returns `{ room, schedule, quiet, override }`, which is useful for dashboards or scripts that need the merged defaults the sender will apply.

## Redis Keys and Streams

- `rooms:next_id` – monotonic counter used by the provisioning script (minimum `PROVISIONING_BASE_ID`).
- `device:{mac}:room` / `room:{id}:device` – bidirectional map between hardware MAC addresses and room ids.
- `room:{id}:cfg` – room schedule JSON (baseline/wake/night/version).
- `room:{id}:desired` – last command published by the sender or the website.
- `room:{id}:reported` – last state acknowledged after the receiver applied PWM.
- `cmd:room:{id}` – command stream consumed by the receiver (trimmed to `~200` entries).
- `state:room:{id}` – acknowledgement stream produced by the receiver (trimmed to `~200` entries).
- `room:{id}:override` – manual override snapshot (enabled flag, version counter, last writer).
- `room:{id}:online` – heartbeat key with TTL `contracts::kHeartbeatTtlSec` so operators can detect offline rooms quickly.

## Useful `redis-cli` Snippets

```bash
# Snapshot of the merged desired state
redis-cli GET room:101:desired

# Watch incoming commands or receiver acknowledgements
redis-cli XREAD BLOCK 0 STREAMS cmd:room:101 $
redis-cli XREAD BLOCK 0 STREAMS state:room:101 $

# Inspect quiet hours + override
redis-cli GET room:101:cfg | jq .
redis-cli GET room:101:override | jq .

# Heartbeat (returns 1 while the receiver is online)
redis-cli EXISTS room:101:online
```

## Wiring Overview

- **Sender control panel**
  - Keep the USB/UART console accessible at `SENDER_CONSOLE_BAUD` so you can respond to `ROOM?` or read logs.
  - Connect the override potentiometer to `OVERRIDE_POT_PIN` (default `A0`) and the toggle button to `OVERRIDE_BUTTON_PIN` (`INPUT_PULLUP`, active LOW by default). Tune `OVERRIDE_ANALOG_*` if your potentiometer range differs.
  - Optional SSD1306 I²C display and status LED pins are configurable through the `SENDER_DISPLAY_*`/`SENDER_STATUS_LED_*` macros.
- **Receiver LED driver**
  - Drive a single MOSFET/transistor via `RECEIVER_LED_PIN` or enable RGB mixing by supplying `RECEIVER_LED_RED/GREEN/BLUE_PIN` plus per-channel mix percentages. Set `RECEIVER_LED_*_ACTIVE_LOW` when driving common-anode fixtures.
  - `RECEIVER_STATUS_LED_PIN` mirrors Wi-Fi/Redis state; set to `-1` to disable or to `LED_BUILTIN` for quick bring-up.
- **Networking**
  - Both boards run in Wi-Fi STA mode on the same 2.4 GHz SSID. Keep them on an isolated lab VLAN when possible and ensure Redis is reachable from that subnet (open TCP 6379 or adjust `REDIS_PORT`).

## Operational Notes

- Both command and state streams are trimmed to `~200` entries to cap Redis memory usage for ~20 rooms.
- The provisioning script guarantees room ids ≥ `PROVISIONING_BASE_ID`, reseeds `room:{id}:desired` when missing, and records the MAC ↔ room mapping for later reuse.
- Manual override versions monotonically increase so the website and hardware never fight—whoever writes last "wins" and the sender reconciles remote changes within ~2 seconds.
- If Redis drops, each ESP falls back to its last known state and resynchronizes automatically once connectivity returns (including clock sync on the sender).
- See `docs/planning.md` for the original requirements, acceptance criteria, and architectural deep dive.
