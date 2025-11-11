# ESP Receiver ↔ ESP Sender Design

This document explains how the two firmware projects (`esp-receiver` and `esp-sender`) behave and how they cooperate through Redis, Wi-Fi, and their shared UART console.

## Shared Data Contract
- Both firmwares include `contracts::Desired`, a `(mode, brightness, ver)` tuple that represents the target LED state. `mode` is `"on"`/`"off"`, `brightness` is a 0–100% percentage, and `ver` is a monotonically increasing change counter used for conflict avoidance.
- Redis key layout (all scoped by a numeric room id):
  - `room:<id>:desired` — sender writes the latest desired state; receiver seeds its state from here on boot.
  - `cmd:room:<id>` — Redis Stream of desired state changes. Sender appends, receiver blocks on `XREAD` to react immediately.
  - `room:<id>:reported` and `state:room:<id>` — receiver mirrors its applied state here (simple key plus history stream) so dashboards can confirm what the room is doing.
  - `room:<id>:online` — receiver sets this key with an expiry heartbeat (`contracts::kHeartbeatTtlSec`) to signal liveness.

## ESP Receiver
1. **Boot & Hardware Init**
   - Initializes Serial at 115200 baud, configures LED PWM on `D4`, and disables Wi-Fi sleep for faster Redis round-trips.
   - Seeds `random()` with the chip id to make Wi-Fi/Redis backoff jitter unique per board.
2. **Connectivity Management**
   - `ensureWifi()` retries `WiFi.begin()` using a small exponential backoff (250→2000 ms) until `WL_CONNECTED`.
   - `ensureRedis()` opens a TCP socket to the configured Redis host (`REDIS_HOST`, `REDIS_PORT`), applies the password, issues `PING`, and resets its internal state whenever the link drops.
3. **Room Provisioning**
   - After Redis is reachable, the receiver gathers its `deviceId` (Wi-Fi MAC) and runs the bundled Lua script via `EVAL`.
   - The script checks `device:<mac>:room`; if missing it allocates the next sequential id (never below `PROVISIONING_BASE_ID`) and seeds `room:<id>:desired` with an “off” state.
   - The receiver caches the returned room id and immediately prints `ROOM:<id>` over UART so that the sender hears it.
4. **State Synchronization**
   - On first join (or after Redis reconnect) it calls `pullSnapshot()` to fetch `room:<id>:desired`, decodes JSON, applies PWM output, and records the applied state to both `room:<id>:reported` and `state:room:<id>`.
   - `pumpStream()` blocks on `XREAD` (1 s) against `cmd:room:<id>`; every payload is decoded, compared against the last applied `ver`, and—if newer—applied and recorded.
5. **Heartbeat & Telemetry**
   - `maintainHeartbeat()` refreshes `room:<id>:online` every `RECEIVER_HEARTBEAT_MS` (default 3 s) with a TTL of 10 s so monitoring code can flag offline rooms.
   - `announceRoom()` reprints `ROOM:<id>` every `ROOM_ANNOUNCE_INTERVAL_MS` (3 s) to keep the sender in sync and to aid debugging over a shared serial console.
6. **Fault Handling**
   - Any Redis failure logs the RESP error, tears down the client, and clears cached room/desired info so the next loop iteration re-provisions or re-snapshots cleanly.

## ESP Sender
1. **Boot & Console Handshake**
   - Starts Serial at `SENDER_CONSOLE_BAUD` (115200 by default) and listens for textual commands.
   - If `ROOM_ID_OVERRIDE` is defined, it uses that immediately; otherwise it repeatedly prints `ROOM?` every 1.5 s until the receiver replies with `ROOM:<id>`. Any time it reads a new room announcement it logs it and resets its cached schedule/version state.
2. **Connectivity & Time**
   - Uses the same Wi-Fi and Redis backoff helpers as the receiver.
   - Configures SNTP via `configTime()` (servers defined in `config.h`) and waits until `time(nullptr)` reports a post-2021 epoch before publishing scheduled commands, logging once on success.
3. **Schedule Management**
   - Room-specific configuration lives at `room:<id>:cfg`. Every `SCHEDULE_REFRESH_MS` (default 30 s) the sender fetches this JSON blob.
   - `RoomSchedule` holds baseline, wake, and night settings with sensible defaults (constants in `config.h`). The JSON parser accepts multiple naming schemes (`baseline.brightness`, `default_brightness`, etc.) to make authoring easier.
   - After each successful load it prints a concise summary (baseline %, wake start/duration/brightness, night window) for operator visibility.
4. **Version Seeding & Publishing**
   - Before emitting anything it reads the current `room:<id>:desired` snapshot so that its `localVer` starts at the last applied version; this prevents stale writes from clobbering newer receiver state.
   - Every `SCHEDULE_PUBLISH_MIN_INTERVAL_MS` (≥1 s) it recomputes the desired brightness using the hydrated schedule and the local clock:
     - Wake window ramps linearly from baseline to the configured peak across the wake duration.
     - Night window enforces the night brightness, even across midnight.
     - Outside those windows it sticks to the baseline brightness.
   - If the computed `(mode, brightness)` differs from the last value it sent, it increments `ver` (if needed), writes the JSON to `room:<id>:desired`, and appends the same payload to `cmd:room:<id>`, trimming the stream to ~200 entries.
5. **Operator Console Commands**
   - `CFG?` echoes the currently loaded schedule summary.
   - `REFRESH` clears the cached config so the next loop fetches Redis immediately.
   - Any unknown command is logged for debugging.
6. **Fault Handling**
   - On Redis errors it logs the `redis_link` status, closes the socket, and resets schedule/version bookkeeping so the system always restarts from a known-good baseline when connectivity returns.

## Receiver ↔ Sender Interaction Flow
1. Both boards share the same UART. The sender polls with `ROOM?` until the receiver (after successful provisioning) prints `ROOM:<id>`.
2. Once the sender knows the room id it:
   - Pulls its schedule/config from Redis.
   - Seeds `localVer` from the existing desired snapshot.
   - Begins publishing scheduled desired states at the configured cadence.
3. Each published desired state is written to both the key and the command stream:
   - The receiver’s `XREAD` consumes the stream entry almost immediately, validates `ver`, and updates the LED PWM accordingly.
   - The receiver mirrors the applied state back into `room:<id>:reported`/`state:room:<id>` so that dashboards or the sender itself can confirm what actually happened.
4. Heartbeats (`room:<id>:online`) allow external monitors—and the sender, if desired—to detect a lost receiver. When a receiver drops offline, the sender will continue to queue commands in Redis until the heartbeat resumes and the receiver resynchronizes via `pullSnapshot()`.
5. Any time the receiver reboots or loses Redis it re-provisions (if needed), announces its room, replays the last desired snapshot, and then drains pending commands—keeping the two devices eventually consistent without manual intervention.

## Summary
- The receiver is the authoritative actuator: it provisions, applies, and reports state while keeping Redis updated with heartbeats and telemetry.
- The sender is a schedule-driven controller: it translates room-level policies into desired states, manages versioning, and streams commands.
- Redis acts as the shared state bus, while the UART link is only used for discovering/confirming the shared room id, making the system robust to restarts and network hiccups.
