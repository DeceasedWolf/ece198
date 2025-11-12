'use strict';

const http = require('node:http');
const net = require('node:net');
const { URL } = require('node:url');

const appConfig = {
  port: Number(process.env.PORT || process.env.WEB_PORT || 3000),
  redisHost: process.env.REDIS_HOST || '127.0.0.1',
  redisPort: Number(process.env.REDIS_PORT || 6379),
  redisPassword: process.env.REDIS_PASSWORD || ''
};
const STREAM_TRIM_LENGTH = 200;

class RedisClient {
  constructor(options) {
    this.host = options.host;
    this.port = options.port;
    this.password = options.password || null;
    this.socket = null;
    this.buffer = Buffer.alloc(0);
    this.queue = [];
    this.ready = false;
    this.connectPromise = null;
    this.closed = false;
  }

  async ensureConnected() {
    if (this.closed) {
      throw new Error('Redis client closed');
    }
    if (this.ready && this.socket) {
      return;
    }
    if (this.connectPromise) {
      return this.connectPromise;
    }
    this.connectPromise = new Promise((resolve, reject) => {
      const socket = net.createConnection(
          { host: this.host, port: this.port },
          async () => {
            try {
              if (this.password) {
                await this.sendCommandInternal(['AUTH', this.password]);
              }
              this.ready = true;
              resolve();
            } catch (err) {
              reject(err);
            }
          });
      this.socket = socket;
      socket.on('data', (chunk) => this.handleData(chunk));
      socket.on('error', (err) => {
        if (!this.closed) {
          console.error('[redis] socket error', err.message);
        }
        this.resetConnection(err);
      });
      socket.on('close', () => {
        this.resetConnection();
      });
    }).finally(() => {
      this.connectPromise = null;
    });
    return this.connectPromise;
  }

  resetConnection(err) {
    this.ready = false;
    if (this.socket) {
      this.socket.destroy();
      this.socket = null;
    }
    const pending = this.queue.splice(0, this.queue.length);
    pending.forEach((waiter) => waiter.reject(err || new Error('Redis connection closed')));
  }

  async sendCommand(args) {
    await this.ensureConnected();
    return this.sendCommandInternal(args);
  }

  sendCommandInternal(args) {
    if (!this.socket) {
      return Promise.reject(new Error('Redis socket unavailable'));
    }
    return new Promise((resolve, reject) => {
      this.queue.push({ resolve, reject });
      this.socket.write(encodeCommand(args));
    });
  }

  handleData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    this.processBuffer();
  }

  processBuffer() {
    while (this.queue.length) {
      const result = parseResp(this.buffer, 0);
      if (!result) {
        return;
      }
      this.buffer = this.buffer.slice(result.nextOffset);
      const waiter = this.queue.shift();
      if (result.error) {
        waiter.reject(result.error);
      } else {
        waiter.resolve(result.value);
      }
    }
  }

  async get(key) {
    return this.sendCommand(['GET', key]);
  }

  async set(key, value) {
    return this.sendCommand(['SET', key, value]);
  }

  close() {
    this.closed = true;
    if (this.socket) {
      this.socket.end();
      this.socket = null;
    }
  }
}

function encodeCommand(parts) {
  const buffers = [];
  buffers.push(Buffer.from(`*${parts.length}\r\n`));
  parts.forEach((part) => {
    const chunk = Buffer.isBuffer(part) ? part : Buffer.from(String(part));
    buffers.push(Buffer.from(`$${chunk.length}\r\n`));
    buffers.push(chunk);
    buffers.push(Buffer.from('\r\n'));
  });
  return Buffer.concat(buffers);
}

function parseResp(buffer, offset = 0) {
  if (!buffer || offset >= buffer.length) {
    return null;
  }
  const prefix = buffer[offset];
  switch (prefix) {
    case 43:  // '+'
      return parseSimpleString(buffer, offset);
    case 45:  // '-'
      return parseError(buffer, offset);
    case 58:  // ':'
      return parseInteger(buffer, offset);
    case 36:  // '$'
      return parseBulkString(buffer, offset);
    case 42:  // '*'
      return parseArray(buffer, offset);
    default:
      return { error: new Error('Unsupported RESP type'), nextOffset: buffer.length };
  }
}

function readLine(buffer, start) {
  const end = buffer.indexOf('\r\n', start);
  if (end === -1) {
    return null;
  }
  return { line: buffer.toString('utf8', start, end), nextOffset: end + 2 };
}

function parseSimpleString(buffer, offset) {
  const result = readLine(buffer, offset + 1);
  if (!result) {
    return null;
  }
  return { value: result.line, nextOffset: result.nextOffset };
}

function parseError(buffer, offset) {
  const result = readLine(buffer, offset + 1);
  if (!result) {
    return null;
  }
  return { error: new Error(result.line), nextOffset: result.nextOffset };
}

function parseInteger(buffer, offset) {
  const result = readLine(buffer, offset + 1);
  if (!result) {
    return null;
  }
  return { value: Number(result.line), nextOffset: result.nextOffset };
}

function parseBulkString(buffer, offset) {
  const header = readLine(buffer, offset + 1);
  if (!header) {
    return null;
  }
  const length = Number(header.line);
  if (Number.isNaN(length)) {
    return { error: new Error('Invalid bulk string length'), nextOffset: header.nextOffset };
  }
  if (length === -1) {
    return { value: null, nextOffset: header.nextOffset };
  }
  const end = header.nextOffset + length;
  if (buffer.length < end + 2) {
    return null;
  }
  const value = buffer.toString('utf8', header.nextOffset, end);
  return { value, nextOffset: end + 2 };
}

function parseArray(buffer, offset) {
  const header = readLine(buffer, offset + 1);
  if (!header) {
    return null;
  }
  const length = Number(header.line);
  if (Number.isNaN(length)) {
    return { error: new Error('Invalid array length'), nextOffset: header.nextOffset };
  }
  if (length === -1) {
    return { value: null, nextOffset: header.nextOffset };
  }
  let cursor = header.nextOffset;
  const items = [];
  for (let i = 0; i < length; i += 1) {
    const item = parseResp(buffer, cursor);
    if (!item) {
      return null;
    }
    if (item.error) {
      return item;
    }
    items.push(item.value);
    cursor = item.nextOffset;
  }
  return { value: items, nextOffset: cursor };
}

const DEFAULT_SCHEDULE = Object.freeze({
  baseline: { brightness: 0 },
  wake: { enabled: true, hour: 7, minute: 0, duration_min: 20, brightness: 100 },
  night: { enabled: true, hour: 22, minute: 0, brightness: 5 },
  version: 0
});
const DEFAULT_DESIRED_STATE = Object.freeze({
  mode: 'off',
  brightness: 0,
  ver: 0
});

function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

function ensureScheduleShape(source) {
  const cfg = typeof source === 'object' && source ? { ...source } : {};
  cfg.baseline = { ...DEFAULT_SCHEDULE.baseline, ...(cfg.baseline || {}) };
  cfg.wake = { ...DEFAULT_SCHEDULE.wake, ...(cfg.wake || {}) };
  cfg.night = { ...DEFAULT_SCHEDULE.night, ...(cfg.night || {}) };
  if (typeof cfg.version !== 'number') {
    if (typeof cfg.cfg_ver === 'number') {
      cfg.version = cfg.cfg_ver;
    } else {
      cfg.version = DEFAULT_SCHEDULE.version;
    }
  }
  return cfg;
}

function readSchedulePayload(payload) {
  if (!payload) {
    return ensureScheduleShape({});
  }
  try {
    const parsed = JSON.parse(payload);
    return ensureScheduleShape(parsed);
  } catch (err) {
    console.warn('[website] invalid schedule JSON, resetting to defaults');
    return deepClone(DEFAULT_SCHEDULE);
  }
}

function formatTime(hour, minute) {
  const h = Number(hour) || 0;
  const m = Number(minute) || 0;
  const hh = h < 10 ? `0${h}` : String(h);
  const mm = m < 10 ? `0${m}` : String(m);
  return `${hh}:${mm}`;
}

function parseTimeInput(value) {
  if (typeof value !== 'string' || !/^\d{2}:\d{2}$/.test(value)) {
    return null;
  }
  const [hourString, minuteString] = value.split(':');
  const hour = Number(hourString);
  const minute = Number(minuteString);
  if (
    Number.isNaN(hour) ||
    Number.isNaN(minute) ||
    hour < 0 ||
    hour > 23 ||
    minute < 0 ||
    minute > 59
  ) {
    return null;
  }
  return { hour, minute };
}

function clampBrightness(value) {
  const num = Number(value);
  if (Number.isNaN(num) || num <= 0) {
    return 0;
  }
  if (num >= 100) {
    return 100;
  }
  return Math.round(num);
}

function readDesiredPayload(payload) {
  if (!payload) {
    return deepClone(DEFAULT_DESIRED_STATE);
  }
  try {
    const parsed = JSON.parse(payload);
    if (typeof parsed !== 'object' || parsed === null) {
      return deepClone(DEFAULT_DESIRED_STATE);
    }
    const mode = parsed.mode === 'on' ? 'on' : 'off';
    const brightness = clampBrightness(parsed.brightness);
    const ver = Number.isInteger(parsed.ver) ? parsed.ver : 0;
    return { mode, brightness, ver };
  } catch (err) {
    console.warn('[website] invalid desired JSON, resetting to defaults');
    return deepClone(DEFAULT_DESIRED_STATE);
  }
}

function quietTimesFromSchedule(schedule) {
  const sleep = formatTime(schedule.night.hour, schedule.night.minute);
  const wake = formatTime(schedule.wake.hour, schedule.wake.minute);
  return { sleep, wake };
}

async function loadSchedule(roomId) {
  const key = roomConfigKey(roomId);
  const payload = await redis.get(key);
  return readSchedulePayload(payload);
}

async function saveSchedule(roomId, schedule) {
  const key = roomConfigKey(roomId);
  if (schedule && Object.prototype.hasOwnProperty.call(schedule, 'cfg_ver')) {
    delete schedule.cfg_ver;
  }
  const serialized = JSON.stringify(schedule);
  await redis.set(key, serialized);
}

function roomConfigKey(roomId) {
  return `room:${roomId}:cfg`;
}

function roomOverrideKey(roomId) {
  return `room:${roomId}:override`;
}

function roomDesiredKey(roomId) {
  return `room:${roomId}:desired`;
}

function roomCommandStream(roomId) {
  return `cmd:room:${roomId}`;
}

async function getOverrideState(roomId) {
  const payload = await redis.get(roomOverrideKey(roomId));
  if (!payload) {
    return { enabled: false, ver: 0, updated_at: 0, source: 'unknown' };
  }
  try {
    const parsed = JSON.parse(payload);
    return {
      enabled: Boolean(parsed.enabled),
      ver: typeof parsed.ver === 'number' ? parsed.ver : 0,
      updated_at: typeof parsed.updated_at === 'number' ? parsed.updated_at : 0,
      source: typeof parsed.source === 'string' ? parsed.source : 'unknown'
    };
  } catch (err) {
    console.warn('[website] invalid override payload, resetting');
    return { enabled: false, ver: 0, updated_at: 0, source: 'unknown' };
  }
}

async function setOverrideState(roomId, enabled, source = 'website') {
  const current = await getOverrideState(roomId);
  const next = {
    enabled: Boolean(enabled),
    ver: Number.isInteger(current.ver) ? current.ver + 1 : 1,
    updated_at: Date.now(),
    source
  };
  await redis.set(roomOverrideKey(roomId), JSON.stringify(next));
  return next;
}

async function sendBrightnessCommand(roomId, brightness, source = 'website') {
  const clamped = clampBrightness(brightness);
  const desiredKey = roomDesiredKey(roomId);
  const payload = await redis.get(desiredKey);
  const current = readDesiredPayload(payload);
  const ver = Number.isInteger(current.ver) ? current.ver + 1 : 1;
  const desired = {
    room: roomId,
    mode: clamped > 0 ? 'on' : 'off',
    brightness: clamped,
    ver,
    source
  };
  const serialized = JSON.stringify(desired);
  await redis.set(desiredKey, serialized);
  const streamKey = roomCommandStream(roomId);
  await redis.sendCommand(['XADD', streamKey, '*', 'p', serialized]);
  await redis.sendCommand([
    'XTRIM',
    streamKey,
    'MAXLEN',
    '~',
    String(STREAM_TRIM_LENGTH)
  ]);
  return desired;
}

async function loadRoomState(roomId) {
  const [schedule, override] = await Promise.all([loadSchedule(roomId), getOverrideState(roomId)]);
  const quiet = quietTimesFromSchedule(schedule);
  return { schedule, quiet, override };
}

async function readRequestBody(req, limitBytes = 4096) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > limitBytes) {
        reject(new Error('Body too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      resolve(Buffer.concat(chunks).toString('utf8'));
    });
    req.on('error', (err) => reject(err));
  });
}

function htmlEscape(value) {
  return String(value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
}

function renderStatusMessage(code) {
  switch (code) {
    case 'quiet-updated':
      return 'Quiet hours saved.';
    case 'override-enabled':
      return 'Override enabled.';
    case 'override-disabled':
      return 'Override disabled.';
    case 'brightness-max':
      return 'Requested full brightness.';
    case 'brightness-min':
      return 'Requested minimum brightness.';
    default:
      return null;
  }
}

function sanitizeFragment(value) {
  if (typeof value !== 'string') {
    return '';
  }
  const trimmed = value.trim();
  if (!trimmed || !/^[A-Za-z0-9_-]+$/.test(trimmed)) {
    return '';
  }
  return trimmed;
}

function roomRedirectLocation(roomId, statusCode, anchor) {
  const base = `/room/${encodeURIComponent(roomId)}`;
  const query = statusCode ? `?status=${encodeURIComponent(statusCode)}` : '';
  const fragment = sanitizeFragment(anchor);
  const hash = fragment ? `#${fragment}` : '';
  return `${base}${query}${hash}`;
}

function renderRoomPage(roomId, state, statusCode) {
  const message = renderStatusMessage(statusCode);
  const overrideLabel = state.override.enabled ? 'enabled' : 'disabled';
  const overrideSource =
      state.override.source === 'unknown' ? '' : ` (via ${htmlEscape(state.override.source)})`;
  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Room ${htmlEscape(roomId)} Lighting</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; margin: 0; padding: 2rem; background: #f9fafb; color: #111827; }
    header { margin-bottom: 2rem; }
    h1 { font-size: 1.5rem; margin: 0 0 0.25rem; }
    section { background: #fff; border-radius: 0.5rem; padding: 1.5rem; box-shadow: 0 1px 2px rgba(0,0,0,0.05); margin-bottom: 1.5rem; }
    form { display: flex; flex-direction: column; gap: 0.75rem; }
    label { display: flex; flex-direction: column; font-weight: 600; font-size: 0.9rem; color: #374151; }
    input[type="time"] { margin-top: 0.25rem; font: inherit; padding: 0.35rem 0.5rem; }
    button { align-self: flex-start; padding: 0.5rem 1rem; font: inherit; border-radius: 0.375rem; border: none; cursor: pointer; }
    button.primary { background: #2563eb; color: #fff; }
    button.secondary { background: #e5e7eb; color: #111827; }
    .status { padding: 0.75rem 1rem; border-radius: 0.375rem; background: #ecfccb; color: #3f6212; margin-bottom: 1rem; font-weight: 600; }
    .override-actions { display: flex; gap: 0.75rem; flex-wrap: wrap; }
    .quick-actions { display: flex; gap: 0.75rem; flex-wrap: wrap; }
    .quick-actions form { flex-direction: row; gap: 0; }
  </style>
</head>
<body>
  <header>
    <h1>Room ${htmlEscape(roomId)}</h1>
    <p>Configure quiet hours and manual override.</p>
  </header>
  ${message ? `<div class="status">${htmlEscape(message)}</div>` : ''}
  <section id="quiet-hours">
    <h2>Quiet Hours</h2>
    <form method="POST" action="/room/${encodeURIComponent(roomId)}/quiet-hours">
      <input type="hidden" name="anchor" value="quiet-hours">
      <label>Quiet hours start
        <input type="time" name="sleep_time" value="${htmlEscape(state.quiet.sleep)}" required>
      </label>
      <label>Wake up time
        <input type="time" name="wake_time" value="${htmlEscape(state.quiet.wake)}" required>
      </label>
      <button type="submit" class="primary">Save Quiet Hours</button>
    </form>
  </section>
  <section id="override">
    <h2>Manual Override</h2>
    <p>Override is currently <strong>${htmlEscape(overrideLabel)}</strong>${overrideSource}.</p>
    <div class="override-actions">
      <form method="POST" action="/room/${encodeURIComponent(roomId)}/override">
        <input type="hidden" name="anchor" value="override">
        <input type="hidden" name="enabled" value="true">
        <button type="submit" class="primary">Enable Override</button>
      </form>
      <form method="POST" action="/room/${encodeURIComponent(roomId)}/override">
        <input type="hidden" name="anchor" value="override">
        <input type="hidden" name="enabled" value="false">
        <button type="submit" class="secondary">Disable Override</button>
      </form>
    </div>
  </section>
  <section id="brightness">
    <h2>Instant Brightness</h2>
    <p>Send an immediate brightness command to the room light.</p>
    <div class="quick-actions">
      <form method="POST" action="/room/${encodeURIComponent(roomId)}/brightness">
        <input type="hidden" name="anchor" value="brightness">
        <input type="hidden" name="level" value="max">
        <button type="submit" class="primary">Full Brightness</button>
      </form>
      <form method="POST" action="/room/${encodeURIComponent(roomId)}/brightness">
        <input type="hidden" name="anchor" value="brightness">
        <input type="hidden" name="level" value="min">
        <button type="submit" class="secondary">Lights Out</button>
      </form>
    </div>
  </section>
</body>
</html>`;
}

function renderIndexPage() {
  return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Lighting Control Rooms</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: #f9fafb; margin: 0; padding: 2rem; color: #111827; }
    h1 { margin-top: 0; }
    form { display: flex; gap: 0.5rem; align-items: center; }
    input[type="text"] { padding: 0.35rem 0.5rem; font: inherit; border-radius: 0.375rem; border: 1px solid #d1d5db; }
    button { padding: 0.5rem 1rem; border-radius: 0.375rem; border: none; background: #2563eb; color: #fff; font: inherit; cursor: pointer; }
  </style>
</head>
<body>
  <h1>Hospital Lighting Control</h1>
  <p>Enter a room id to manage quiet hours and override state.</p>
  <form method="GET" action="/room/">
    <input type="text" name="room" placeholder="Room ID (e.g. 101)" required>
    <button type="submit">Open Room</button>
  </form>
</body>
</html>`;
}

function matchRoomRoute(pathname) {
  const match = pathname.match(/^\/room\/([^/]+)(?:\/(quiet-hours|override|brightness))?\/?$/);
  if (!match) {
    return null;
  }
  const roomId = decodeURIComponent(match[1]);
  if (!isValidRoomId(roomId)) {
    return null;
  }
  return { roomId, action: match[2] || null };
}

function matchApiRoute(pathname) {
  const match = pathname.match(/^\/api\/rooms\/([^/]+)\/?$/);
  if (!match) {
    return null;
  }
  const roomId = decodeURIComponent(match[1]);
  if (!isValidRoomId(roomId)) {
    return null;
  }
  return { roomId };
}

function isValidRoomId(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_-]+$/.test(value);
}

async function handleRequest(req, res) {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const pathname = url.pathname;

  if (req.method === 'GET' && pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(renderIndexPage());
    return;
  }

  if (req.method === 'GET' && (pathname === '/room' || pathname === '/room/')) {
    const roomId = url.searchParams.get('room');
    if (isValidRoomId(roomId)) {
      res.writeHead(302, { Location: `/room/${encodeURIComponent(roomId)}` });
      res.end();
    } else {
      res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Invalid room id');
    }
    return;
  }

  const roomRoute = matchRoomRoute(pathname);
  if (roomRoute) {
    const { roomId, action } = roomRoute;
    if (req.method === 'GET' && !action) {
      const state = await loadRoomState(roomId);
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      res.end(renderRoomPage(roomId, state, url.searchParams.get('status')));
      return;
    }
    if (req.method === 'POST' && action === 'quiet-hours') {
      const body = await readRequestBody(req);
      const params = new URLSearchParams(body);
      const sleep = parseTimeInput(params.get('sleep_time'));
      const wake = parseTimeInput(params.get('wake_time'));
      if (!sleep || !wake) {
        res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('Invalid time supplied');
        return;
      }
      const schedule = await loadSchedule(roomId);
      schedule.night.enabled = true;
      schedule.night.hour = sleep.hour;
      schedule.night.minute = sleep.minute;
      schedule.wake.enabled = true;
      schedule.wake.hour = wake.hour;
      schedule.wake.minute = wake.minute;
      schedule.version = Number.isInteger(schedule.version) ? schedule.version + 1 : 1;
      await saveSchedule(roomId, schedule);
      const location = roomRedirectLocation(roomId, 'quiet-updated', params.get('anchor'));
      res.writeHead(303, { Location: location });
      res.end();
      return;
    }
    if (req.method === 'POST' && action === 'override') {
      const body = await readRequestBody(req);
      const params = new URLSearchParams(body);
      const enabledParam = params.get('enabled');
      if (enabledParam !== 'true' && enabledParam !== 'false') {
        res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('Enabled flag missing');
        return;
      }
      const enabled = enabledParam === 'true';
      await setOverrideState(roomId, enabled, 'website');
      const statusCode = enabled ? 'override-enabled' : 'override-disabled';
      const location = roomRedirectLocation(roomId, statusCode, params.get('anchor'));
      res.writeHead(303, { Location: location });
      res.end();
      return;
    }
    if (req.method === 'POST' && action === 'brightness') {
      const body = await readRequestBody(req);
      const params = new URLSearchParams(body);
      const level = params.get('level');
      let brightnessTarget;
      let statusCode;
      if (level === 'max') {
        brightnessTarget = 100;
        statusCode = 'brightness-max';
      } else if (level === 'min') {
        brightnessTarget = 0;
        statusCode = 'brightness-min';
      } else {
        res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
        res.end('Unknown brightness level');
        return;
      }
      await sendBrightnessCommand(roomId, brightnessTarget, 'website');
      const location = roomRedirectLocation(roomId, statusCode, params.get('anchor'));
      res.writeHead(303, { Location: location });
      res.end();
      return;
    }
  }

  const apiRoute = matchApiRoute(pathname);
  if (apiRoute && req.method === 'GET') {
    const data = await loadRoomState(apiRoute.roomId);
    res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(JSON.stringify({ room: apiRoute.roomId, ...data }));
    return;
  }

  if (req.method === 'GET' && pathname === '/healthz') {
    await redis.sendCommand(['PING']);
    res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('ok');
    return;
  }

  res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
  res.end('Not Found');
}

const redis = new RedisClient({
  host: appConfig.redisHost,
  port: appConfig.redisPort,
  password: appConfig.redisPassword
});

const server = http.createServer((req, res) => {
  handleRequest(req, res).catch((err) => {
    console.error('[website] request failed', err);
    if (!res.headersSent) {
      res.writeHead(500, { 'Content-Type': 'text/plain; charset=utf-8' });
    }
    res.end('Internal Server Error');
  });
});

server.listen(appConfig.port, () => {
  console.log(
      `[website] listening on port ${appConfig.port} (Redis ${appConfig.redisHost}:${appConfig.redisPort})`);
});

process.on('SIGINT', () => {
  console.log('\n[website] shutting down');
  redis.close();
  server.close(() => process.exit(0));
});
