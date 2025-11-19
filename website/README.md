# Website Controls

This package contains the dependency-free Node.js server that powers the quiet-hours, manual-override, and sound-warning experience for every room. It speaks Redis RESP directly over TCP (no `node-redis` dependency) so the only runtime requirement is Node 18+.

## Prerequisites

- Node.js 18 or newer.
- Network access to the same Redis instance the ESP firmware targets use (usually the `docker compose` stack that listens on `localhost:6379` during development).

## Configuration

All configuration is optional and provided through environment variables:

| Variable                  | Default     | Description                                                                 |
| ------------------------- | ----------- | --------------------------------------------------------------------------- |
| `PORT` / `WEB_PORT`       | `3000`      | HTTP listen port (both names are accepted; `PORT` takes precedence).        |
| `REDIS_HOST`              | `127.0.0.1` | Redis hostname or IP address.                                               |
| `REDIS_PORT`              | `6379`      | Redis TCP port.                                                             |
| `REDIS_PASSWORD`          | *(empty)*   | Password for Redis `AUTH`, if enabled.                                      |

## Running

```bash
cd website
npm start
```

No `npm install` step is necessary because the project has zero third-party dependencies. Once the server is running, open `http://localhost:3000/` and enter a room id (for example `101`).

## UI Features

- **Home page (`/`)** – simple form that redirects to `/room/{id}` after validating the room id.
- **Room page (`/room/{id}`)** – renders quiet-hour forms, override status, and brightness quick actions. Status messages confirm each successful change.
  - Quiet hours form rewrites `room:{id}:cfg`, ensuring `night.enabled` and `wake.enabled` are both true and forcing `night.brightness` to 0 for quiet mode.
  - Manual override buttons map to the same Redis key (`room:{id}:override`) that the hardware button updates, including the version counter so whichever side wrote last wins.
  - Instant brightness buttons are meant for testing—they immediately write `room:{id}:desired` and append to `cmd:room:{id}` so the receiver reacts even if the sender is offline.
  - Quiet-hour sound monitoring card shows the latest `room:{id}:latest_warning` payload (timestamp, decibels, threshold, quiet flag) so staff know when noise exceeded the configured limit.

## API Endpoints

Every form submit results in a `303 See Other` redirect back to `/room/{id}` with a status query string so the user sees an inline confirmation. You can also call the backing endpoints directly:

| Method & Path                  | Description                                                                                   | Redis keys touched                               |
| ------------------------------ | --------------------------------------------------------------------------------------------- | ------------------------------------------------ |
| `GET /`                       | Landing page with the room selector.                                                          | *(none)*                                         |
| `GET /room/{id}`              | Renders the HTML UI by merging defaults with `room:{id}:cfg` and `room:{id}:override`.        | `room:{id}:cfg`, `room:{id}:override`, `room:{id}:desired`, `room:{id}:latest_warning` |
| `POST /room/{id}/quiet-hours` | Form fields `sleep_time=HH:MM`, `wake_time=HH:MM`. Updates the stored schedule JSON.          | `room:{id}:cfg`                                  |
| `POST /room/{id}/override`    | Form field `enabled=true|false`. Bumps the override version and records `source=website`.     | `room:{id}:override`                             |
| `POST /room/{id}/brightness`  | Form field `level=max|min`. Rewrites `room:{id}:desired`, emits `cmd:room:{id}`, trims stream.| `room:{id}:desired`, `cmd:room:{id}`             |
| `GET /api/rooms/{id}`         | Returns JSON `{ room, schedule, quiet, override, warning }` (schedule already merged with defaults).   | Same as the room page                            |
| `GET /healthz`                | Performs a Redis `PING` to ensure the RESP connection is healthy.                            | *(none beyond PING)*                             |

All endpoints reject invalid room ids and return `404` if the route does not exist. Quiet-hours and override payloads match the shapes expected by `esp-sender`, so the devices see the changes within ~2 seconds of submission.

## Data Model

- Quiet hours → `room:{id}:cfg` JSON (`baseline`, `wake`, `night`, `version`). The website only mutates the quiet-hour fields; other fields survive untouched because the server merges the stored payload with the defaults before writing.
- Manual override → `room:{id}:override` JSON (`enabled`, incrementing `ver`, `updated_at` epoch ms, `source="website"`).
- Instant brightness → `room:{id}:desired` snapshot (includes `mode`, `brightness`, `ver`, `room`, `source`) plus `cmd:room:{id}` stream entries with the serialized payload tagged as field `p`.
- Quiet-hour warning log → `room:{id}:latest_warning` JSON (`decibels`, `threshold`, `captured_at`, `quiet`, `source`). The receiver rewrites this whenever the quiet-hour sound sensor crosses the configured threshold, and the website mirrors the payload in both HTML and JSON responses.

Use `curl` or any HTTP client if you need to automate changes:

```bash
curl -X POST http://localhost:3000/room/101/quiet-hours \
  -d 'sleep_time=21:00' \
  -d 'wake_time=06:30'

curl -X POST http://localhost:3000/room/101/override \
  -d 'enabled=true'
```

Because this server talks to the same Redis instance as the ESP firmware, any change made through the UI is reflected on the physical hardware (and vice versa) without additional glue code.
