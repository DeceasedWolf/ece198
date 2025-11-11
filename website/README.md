# Website Controls

This directory contains a dependency-free Node.js server that exposes a simple per-room UI for
updating quiet hours and toggling the manual override (the same state that exists on the physical
control panel button).

## Prerequisites

- Node.js 18+ (needed for the built-in `fetch`-free HTTP/RESP implementation).
- Access to the same Redis instance that the ESP sender/receiver use.

## Configuration

The server reads a few environment variables (all optional):

| Variable        | Default        | Description                              |
| --------------- | -------------- | ---------------------------------------- |
| `PORT`          | `3000`         | Port that the HTTP server listens on.    |
| `REDIS_HOST`    | `127.0.0.1`    | Redis hostname or IP.                    |
| `REDIS_PORT`    | `6379`         | Redis TCP port.                          |
| `REDIS_PASSWORD`| *(empty)*      | Password if Redis AUTH is enabled.       |

## Running

```bash
cd website
npm start
```

Open `http://localhost:3000/` and enter a room id. Each room has its own URL
(`http://localhost:3000/room/101`, etc.) where occupants can:

- Set their preferred quiet-hours start (lights dim) and wake time (lights brighten).
- Enable/disable the manual override, mirroring the hardware button.

### API

The HTML UI sits on top of a small API that can also be scripted:

- `GET /api/rooms/{id}` – returns the merged schedule plus the current override snapshot.
- `POST /room/{id}/quiet-hours` – accepts form data (`sleep_time=HH:MM`, `wake_time=HH:MM`).
- `POST /room/{id}/override` – accepts form data (`enabled=true|false`).

All changes are persisted in Redis under `room:{id}:cfg` (quiet hours) and `room:{id}:override`
(override state) so that the ESP sender/receiver can observe them.
