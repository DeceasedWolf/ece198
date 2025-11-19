#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"
#include "contracts.hpp"
#include "redis_link.hpp"

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

namespace {

constexpr uint16_t kStreamTrimLen = 200;
constexpr uint32_t kXreadBlockMs = 1000;
constexpr uint16_t kRedisTimeoutMs = 1500;

#ifndef RECEIVER_LED_PIN
#define RECEIVER_LED_PIN D4
#endif
#ifndef RECEIVER_LED_ACTIVE_LOW
#define RECEIVER_LED_ACTIVE_LOW 0
#endif
#ifndef RECEIVER_LED_RED_PIN
#define RECEIVER_LED_RED_PIN -1
#endif
#ifndef RECEIVER_LED_GREEN_PIN
#define RECEIVER_LED_GREEN_PIN -1
#endif
#ifndef RECEIVER_LED_BLUE_PIN
#define RECEIVER_LED_BLUE_PIN -1
#endif
#ifndef RECEIVER_LED_RED_PERCENT
#define RECEIVER_LED_RED_PERCENT 0
#endif
#ifndef RECEIVER_LED_GREEN_PERCENT
#define RECEIVER_LED_GREEN_PERCENT 0
#endif
#ifndef RECEIVER_LED_BLUE_PERCENT
#define RECEIVER_LED_BLUE_PERCENT 0
#endif
#ifndef RECEIVER_STATUS_LED_PIN
#define RECEIVER_STATUS_LED_PIN LED_BUILTIN
#endif
#ifndef RECEIVER_STATUS_LED_ACTIVE_LOW
#define RECEIVER_STATUS_LED_ACTIVE_LOW 1
#endif

#ifndef RECEIVER_CFG_REFRESH_MS
#define RECEIVER_CFG_REFRESH_MS 60000
#endif
#ifndef RECEIVER_SOUND_SENSOR_PIN
#define RECEIVER_SOUND_SENSOR_PIN -1
#endif
#ifndef RECEIVER_SOUND_SAMPLE_INTERVAL_MS
#define RECEIVER_SOUND_SAMPLE_INTERVAL_MS 200
#endif
#ifndef RECEIVER_SOUND_AVERAGE_SAMPLES
#define RECEIVER_SOUND_AVERAGE_SAMPLES 4
#endif
#ifndef RECEIVER_SOUND_SENSOR_MIN_DB
#define RECEIVER_SOUND_SENSOR_MIN_DB 30.0f
#endif
#ifndef RECEIVER_SOUND_SENSOR_MAX_DB
#define RECEIVER_SOUND_SENSOR_MAX_DB 110.0f
#endif
#ifndef RECEIVER_SOUND_WARNING_THRESHOLD_DB
#define RECEIVER_SOUND_WARNING_THRESHOLD_DB 80.0f
#endif
#ifndef RECEIVER_SOUND_WARNING_COOLDOWN_MS
#define RECEIVER_SOUND_WARNING_COOLDOWN_MS 60000
#endif
#ifndef RECEIVER_LED_HAS_RGB
#if (RECEIVER_LED_RED_PIN >= 0) || (RECEIVER_LED_GREEN_PIN >= 0) || (RECEIVER_LED_BLUE_PIN >= 0)
#define RECEIVER_LED_HAS_RGB 1
#else
#define RECEIVER_LED_HAS_RGB 0
#endif
#endif

static_assert(RECEIVER_LED_RED_PERCENT >= 0 && RECEIVER_LED_RED_PERCENT <= 100,
              "RECEIVER_LED_RED_PERCENT must be between 0 and 100");
static_assert(RECEIVER_LED_GREEN_PERCENT >= 0 && RECEIVER_LED_GREEN_PERCENT <= 100,
              "RECEIVER_LED_GREEN_PERCENT must be between 0 and 100");
static_assert(RECEIVER_LED_BLUE_PERCENT >= 0 && RECEIVER_LED_BLUE_PERCENT <= 100,
              "RECEIVER_LED_BLUE_PERCENT must be between 0 and 100");

constexpr bool kLedActiveLow = RECEIVER_LED_ACTIVE_LOW;
constexpr int8_t kStatusLedPin = RECEIVER_STATUS_LED_PIN;
constexpr bool kStatusLedActiveLow = RECEIVER_STATUS_LED_ACTIVE_LOW;
constexpr bool kStatusLedEnabled = (RECEIVER_STATUS_LED_PIN >= 0);

/**
 * Converts a percentage value into the current PWM range for analogWrite.
 */
constexpr uint16_t percentToDuty(uint8_t percent) {
  return static_cast<uint16_t>((static_cast<uint32_t>(percent) * PWMRANGE) / 100);
}

struct LedChannel {
  uint8_t pin;
  uint16_t maxDuty;
};

#if RECEIVER_LED_HAS_RGB
constexpr LedChannel kLedChannels[] = {
#if RECEIVER_LED_RED_PIN >= 0
    {static_cast<uint8_t>(RECEIVER_LED_RED_PIN),
     percentToDuty(static_cast<uint8_t>(RECEIVER_LED_RED_PERCENT))},
#endif
#if RECEIVER_LED_GREEN_PIN >= 0
    {static_cast<uint8_t>(RECEIVER_LED_GREEN_PIN),
     percentToDuty(static_cast<uint8_t>(RECEIVER_LED_GREEN_PERCENT))},
#endif
#if RECEIVER_LED_BLUE_PIN >= 0
    {static_cast<uint8_t>(RECEIVER_LED_BLUE_PIN),
     percentToDuty(static_cast<uint8_t>(RECEIVER_LED_BLUE_PERCENT))},
#endif
};
#else
constexpr LedChannel kLedChannels[] = {
    {static_cast<uint8_t>(RECEIVER_LED_PIN), percentToDuty(100)},
};
#endif

constexpr size_t kLedChannelCount = sizeof(kLedChannels) / sizeof(kLedChannels[0]);
static_assert(kLedChannelCount > 0, "At least one LED channel must be configured");

/**
 * Reports whether the status LED shares the same physical pin as a driver channel.
 */
constexpr bool statusLedSharesDriverPin() {
#if RECEIVER_LED_HAS_RGB
  return false
#if RECEIVER_LED_RED_PIN >= 0
         || (RECEIVER_STATUS_LED_PIN == RECEIVER_LED_RED_PIN)
#endif
#if RECEIVER_LED_GREEN_PIN >= 0
         || (RECEIVER_STATUS_LED_PIN == RECEIVER_LED_GREEN_PIN)
#endif
#if RECEIVER_LED_BLUE_PIN >= 0
         || (RECEIVER_STATUS_LED_PIN == RECEIVER_LED_BLUE_PIN)
#endif
      ;
#else
  return (RECEIVER_STATUS_LED_PIN == RECEIVER_LED_PIN);
#endif
}

constexpr bool kStatusLedSharesDriver = kStatusLedEnabled && statusLedSharesDriverPin();
constexpr bool kStatusLedControllable = kStatusLedEnabled && !kStatusLedSharesDriver;
constexpr unsigned long kStatusLedBlinkIntervalMs = 400;
constexpr unsigned long kQuietConfigRefreshMs = RECEIVER_CFG_REFRESH_MS;
constexpr bool kSoundSensorEnabled = (RECEIVER_SOUND_SENSOR_PIN >= 0);
constexpr unsigned long kSoundSampleIntervalMs = RECEIVER_SOUND_SAMPLE_INTERVAL_MS;
constexpr uint8_t kSoundSampleCount =
    (RECEIVER_SOUND_AVERAGE_SAMPLES >= 1 ? RECEIVER_SOUND_AVERAGE_SAMPLES : 1);
constexpr uint16_t kMinutesPerDay = 24 * 60;
constexpr time_t kMinValidEpoch = 1609459200;
constexpr float kSoundMinDb = RECEIVER_SOUND_SENSOR_MIN_DB;
constexpr float kSoundMaxDb = RECEIVER_SOUND_SENSOR_MAX_DB;
constexpr float kSoundThresholdDb = RECEIVER_SOUND_WARNING_THRESHOLD_DB;
constexpr unsigned long kSoundWarningCooldownMs = RECEIVER_SOUND_WARNING_COOLDOWN_MS;
constexpr float kSoundAdcMax = 1023.0f;
static_assert(kSoundSampleCount >= 1, "RECEIVER_SOUND_AVERAGE_SAMPLES must be >= 1");

/**
 * Records retry scheduling information for Wi-Fi/Redis links.
 */
struct Backoff {
  unsigned long nextMs = 0;
  uint8_t slot = 0;
  bool ready(unsigned long now) const { return now >= nextMs; }
  void schedule(unsigned long now) {
    static const uint16_t kSteps[] = {250, 500, 1000, 2000};
    uint8_t idx = slot < 3 ? slot : 3;
    nextMs = now + kSteps[idx] + random(0, 200);
    if (slot < 3) {
      ++slot;
    }
  }
  void reset() {
    nextMs = 0;
    slot = 0;
  }
};

WiFiClient redisClient;
RedisLink redis(redisClient);

Backoff wifiBackoff;
Backoff redisBackoff;

String roomId;
String deviceId;
contracts::Desired lastDesired;
bool hasDesired = false;
uint32_t lastAppliedVer = 0;
String lastStreamId;
bool streamCursorValid = false;
unsigned long lastHeartbeatMs = 0;
unsigned long lastAnnounceMs = 0;
String jsonScratch;
bool wifiAnnounced = false;
String warningScratch;
bool timeConfigured = false;
bool timeAnnounced = false;
unsigned long lastTimeSyncAttemptMs = 0;

/** Parsed quiet-hours schedule pulled from Redis. */
struct QuietHoursWindow {
  bool enabled = false;
  uint16_t startMinutes = 0;
  uint16_t endMinutes = 0;
  uint32_t version = 0;
};

QuietHoursWindow quietWindow;
bool quietWindowLoaded = false;
unsigned long lastQuietFetchMs = 0;
unsigned long lastSoundSampleMs = 0;
unsigned long lastWarningPublishedMs = 0;

enum class StatusLedMode { Off, Solid, Blink };
StatusLedMode statusLedMode = StatusLedMode::Off;
bool statusLedBlinkState = true;
int8_t statusLedAppliedState = -1;
unsigned long statusLedLastToggleMs = 0;

/**
 * Parses a Desired payload from JSON while logging helpful errors for operators.
 */
bool decodeDesiredJson(const String &payload,
                       contracts::Desired &desired,
                       const __FlashStringHelper *context) {
  StaticJsonDocument<contracts::kDesiredJsonCapacity> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print(F("[desired] "));
    if (context) {
      Serial.print(context);
      Serial.print(' ');
    }
    Serial.print(F("json error: "));
    Serial.println(err.c_str());
    Serial.print(F("[desired] payload: "));
    Serial.println(payload);
    return false;
  }
  JsonVariantConst modeVar = doc["mode"];
  const char *modePtr = nullptr;
  String modeStr;
  if (modeVar.is<const char *>()) {
    modePtr = modeVar.as<const char *>();
  } else if (modeVar.is<String>()) {
    modeStr = modeVar.as<String>();
    modePtr = modeStr.c_str();
  }
  if (!contracts::copyMode(modePtr, desired)) {
    Serial.print(F("[desired] "));
    if (context) {
      Serial.print(context);
      Serial.print(' ');
    }
    Serial.print(F("invalid mode: "));
    Serial.println(modePtr ? modePtr : "(null)");
    Serial.print(F("[desired] payload: "));
    Serial.println(payload);
    return false;
  }
  desired.brightness = doc["brightness"] | desired.brightness;
  contracts::clampBrightness(desired);
  desired.ver = doc["ver"] | desired.ver;
  return true;
}

/**
 * Lua script invoked via EVAL to map MAC addresses to room ids in Redis.
 */
const char kProvisionScript[] PROGMEM = R"lua(
local dev = ARGV[1]
local base = tonumber(ARGV[2]) or 100
local rid = redis.call('GET','device:'..dev..':room')
if rid then return rid end
local next_id = redis.call('INCR','rooms:next_id')
if next_id < base then
  next_id = base
  redis.call('SET','rooms:next_id',base)
end
rid = tostring(next_id)
redis.call('SET','device:'..dev..':room',rid)
redis.call('SET','room:'..rid..':device',dev)
if redis.call('EXISTS','room:'..rid..':desired') == 0 then
  redis.call('SET','room:'..rid..':desired','{"mode":"off","brightness":0,"ver":0}')
end
return rid
)lua";

/** Emits a tagged receiver log line. */
void logInfo(const __FlashStringHelper *msg) {
  Serial.print(F("[receiver] "));
  Serial.println(msg);
}

/** Logs the most recent Redis error string with additional context. */
void logRedisFailure(const __FlashStringHelper *where) {
  Serial.print(F("[redis] "));
  Serial.print(where);
  Serial.print(F(": "));
  Serial.println(redis.lastError());
}

/** Returns the corrected duty cycle when the output is active-low. */
uint16_t applyPolarity(uint16_t duty, bool activeLow) {
  return activeLow ? (PWMRANGE - duty) : duty;
}

/** Writes a PWM duty cycle to the supplied LED pin. */
void writeLedDuty(uint8_t pin, uint16_t duty, bool activeLow) {
  analogWrite(pin, applyPolarity(duty, activeLow));
}

/** Drives the optional status LED without fighting LED channels that share the pin. */
void setStatusLed(bool on) {
  if (!kStatusLedControllable) {
    return;
  }
  int8_t target = on ? 1 : 0;
  if (statusLedAppliedState == target) {
    return;
  }
  statusLedAppliedState = target;
  writeLedDuty(static_cast<uint8_t>(kStatusLedPin), on ? PWMRANGE : 0, kStatusLedActiveLow);
}

/** Updates the status LED pattern based on Wi-Fi/Redis health. */
void updateStatusLed(unsigned long now) {
  if (!kStatusLedControllable) {
    return;
  }
  StatusLedMode desired = StatusLedMode::Off;
  if (WiFi.status() == WL_CONNECTED) {
    desired = redis.connected() ? StatusLedMode::Blink : StatusLedMode::Solid;
  }
  if (desired != statusLedMode) {
    statusLedMode = desired;
    statusLedBlinkState = true;
    statusLedLastToggleMs = now;
    if (desired == StatusLedMode::Blink) {
      setStatusLed(true);
    } else {
      setStatusLed(desired == StatusLedMode::Solid);
    }
    return;
  }
  if (desired == StatusLedMode::Blink) {
    if ((now - statusLedLastToggleMs) >= kStatusLedBlinkIntervalMs) {
      statusLedBlinkState = !statusLedBlinkState;
      statusLedLastToggleMs = now;
      setStatusLed(statusLedBlinkState);
    }
  } else if (desired == StatusLedMode::Solid) {
    setStatusLed(true);
  } else {
    setStatusLed(false);
  }
}

/** Clears cached room-specific state so resyncs start from scratch. */
void resetRoomState(bool dropRoomId = true) {
  hasDesired = false;
  lastAppliedVer = 0;
  lastHeartbeatMs = 0;
  lastAnnounceMs = 0;
  streamCursorValid = false;
  lastStreamId.remove(0);
  quietWindow = QuietHoursWindow();
  quietWindowLoaded = false;
  lastQuietFetchMs = 0;
  lastSoundSampleMs = 0;
  lastWarningPublishedMs = 0;
  if (dropRoomId) {
    roomId.remove(0);
  }
}

/** Tears down the Redis link after logging the failure. */
void dropRedis(const __FlashStringHelper *context) {
  logRedisFailure(context);
  redis.stop();
  resetRoomState();
}

/**
 * Initializes the stream cursor so the receiver resumes consuming at the tail.
 */
bool primeStreamCursor() {
  if (!roomId.length()) {
    return false;
  }
  String tailId;
  if (!redis.streamTailId(contracts::stream_cmd(roomId), tailId)) {
    return false;
  }
  if (tailId.length()) {
    lastStreamId = tailId;
  } else {
    lastStreamId = F("0-0");
  }
  streamCursorValid = true;
  return true;
}

/** Prints the current Wi-Fi status plus IP information. */
void logWifiSnapshot(const __FlashStringHelper *prefix) {
  Serial.print(F("[wifi] "));
  if (prefix) {
    Serial.print(prefix);
    Serial.print(' ');
  }
  wl_status_t status = WiFi.status();
  Serial.print(F("status="));
  Serial.print(static_cast<int>(status));
  Serial.print(F(" ip="));
  Serial.print(WiFi.localIP());
  Serial.print(F(" gw="));
  Serial.print(WiFi.gatewayIP());
  Serial.print(F(" rssi="));
  Serial.println(WiFi.RSSI());
}

/** Connects to Wi-Fi STA mode, retrying until the link is healthy. */
void connectWifiBlocking() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(500);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[wifi] blocking connect to "));
  Serial.println(F(WIFI_SSID));
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(F("[wifi] status="));
    Serial.println(static_cast<int>(WiFi.status()));
    delay(500);
    if (millis() - start > 20000) {
      Serial.println(F("[wifi] retrying blocking connect"));
      WiFi.disconnect(false);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }
  logWifiSnapshot(F("connected (blocking)"));
}

/** Keeps Wi-Fi associated, forcing a blocking reconnect when needed. */
bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiBackoff.reset();
    if (!wifiAnnounced) {
      logWifiSnapshot(F("connected"));
      wifiAnnounced = true;
    }
    return true;
  }
  wifiAnnounced = false;
  connectWifiBlocking();
  wifiBackoff.reset();
  wifiAnnounced = true;
  return true;
}

/** Maintains the Redis TCP connection, authenticating and pinging before use. */
bool ensureRedis() {
  if (redis.connected()) {
    redisBackoff.reset();
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  unsigned long now = millis();
  if (!redisBackoff.ready(now)) {
    return false;
  }
  redis.stop();
  redisClient.stop();
  Serial.printf("[redis] connect %s:%u\n", REDIS_HOST, static_cast<unsigned>(REDIS_PORT));
  if (!redisClient.connect(REDIS_HOST, REDIS_PORT)) {
    Serial.println("[redis] tcp connect failed");
    redisBackoff.schedule(now);
    return false;
  }
  redis.setTimeout(kRedisTimeoutMs);
  redisClient.setNoDelay(true);
  if (!redis.auth(REDIS_PASSWORD)) {
    Serial.println("[redis] auth failed");
    redisBackoff.schedule(now);
    dropRedis(F("auth"));
    return false;
  }
  if (!redis.ping()) {
    Serial.println("[redis] ping failed");
    redisBackoff.schedule(now);
    dropRedis(F("ping"));
    return false;
  }
  Serial.println("[redis] connected");
  redisBackoff.reset();
  resetRoomState();
  return true;
}

/** Periodically prints `ROOM:<id>` so the sender can copy/paste the assignment. */
void announceRoom(bool force) {
  if (!roomId.length()) {
    return;
  }
  unsigned long now = millis();
  if (!force && (now - lastAnnounceMs) < ROOM_ANNOUNCE_INTERVAL_MS) {
    return;
  }
  Serial.print(F("ROOM:"));
  Serial.println(roomId);
  lastAnnounceMs = now;
}

/**
 * Runs the provisioning script so each receiver learns which room it controls.
 */
bool provisionRoom() {
  if (!redis.connected()) {
    return false;
  }
  String rid;
  Serial.println("[redis] provisioning room");
  if (!redis.evalRoomScript(FPSTR(kProvisionScript), deviceId, PROVISIONING_BASE_ID, rid)) {
    Serial.println("[redis] provision failed");
    dropRedis(F("provision"));
    return false;
  }
  Serial.print("[redis] provisioned room ");
  Serial.println(rid);
  if (!rid.length()) {
    return false;
  }
  if (rid != roomId) {
    roomId = rid;
    resetRoomState(false);
  }
  announceRoom(true);
  return true;
}

/**
 * Applies a Desired snapshot to every configured LED channel.
 */
void applyPwm(const contracts::Desired &desired) {
  uint16_t brightnessDuty = 0;
  if (strcmp(desired.mode, "on") == 0 && desired.brightness > 0) {
    brightnessDuty = map(desired.brightness, 0, 100, 0, PWMRANGE);
  }
  for (const auto &channel : kLedChannels) {
    uint32_t duty = (static_cast<uint32_t>(channel.maxDuty) * brightnessDuty) / PWMRANGE;
    writeLedDuty(channel.pin, static_cast<uint16_t>(duty), kLedActiveLow);
  }
  Serial.printf("[pwm] duty=%u mode=%s brightness=%u\n",
                static_cast<unsigned>(brightnessDuty),
                desired.mode,
                static_cast<unsigned>(desired.brightness));
}

/**
 * Writes the applied Desired snapshot to both the reported key and stream.
 */
bool recordState(const String &json) {
  if (!redis.set(contracts::key_reported(roomId), json)) {
    dropRedis(F("set reported"));
    return false;
  }
  if (!redis.xaddJson(contracts::stream_state(roomId), json)) {
    dropRedis(F("xadd state"));
    return false;
  }
  redis.xtrimApprox(contracts::stream_state(roomId), kStreamTrimLen);
  return true;
}

/**
 * Loads the latest Desired snapshot from Redis and applies it immediately.
 */
bool pullSnapshot() {
  bool isNull = false;
  String stored;
  if (!redis.get(contracts::key_desired(roomId), stored, &isNull)) {
    dropRedis(F("get desired"));
    return false;
  }
  if (isNull || !stored.length()) {
    stored = F("{\"mode\":\"off\",\"brightness\":0,\"ver\":0}");
  }
  contracts::Desired desired;
  if (!decodeDesiredJson(stored, desired, F("snapshot"))) {
    desired = contracts::Desired();
  }
  if (!contracts::encodeDesired(desired, &roomId, jsonScratch)) {
    return false;
  }
  applyPwm(desired);
  lastDesired = desired;
  lastAppliedVer = desired.ver;
  hasDesired = true;
  if (!recordState(jsonScratch)) {
    return false;
  }
  streamCursorValid = false;
  lastStreamId.remove(0);
  return true;
}

/** Decodes a streamed command and applies it when the version is newer. */
void handlePayload(const String &payload) {
  contracts::Desired desired = lastDesired;
  if (!decodeDesiredJson(payload, desired, F("stream"))) {
    return;
  }
  if (desired.ver <= lastAppliedVer) {
    return;
  }
  if (!contracts::encodeDesired(desired, &roomId, jsonScratch)) {
    return;
  }
  applyPwm(desired);
  lastDesired = desired;
  lastAppliedVer = desired.ver;
  hasDesired = true;
  recordState(jsonScratch);
}

/**
 * Continuously blocks on `XREAD` so new commands are applied with minimal latency.
 */
void pumpStream() {
  if (!roomId.length() || !hasDesired) {
    return;
  }
  if (!streamCursorValid) {
    if (!primeStreamCursor()) {
      dropRedis(F("stream tail"));
      return;
    }
  }
  String payload;
  String entryId;
  const String cursor = lastStreamId.length() ? lastStreamId : String(F("0-0"));
  if (redis.xreadLatest(contracts::stream_cmd(roomId), kXreadBlockMs, cursor, entryId, payload)) {
    Serial.print("[stream] id: ");
    Serial.print(entryId);
    Serial.print(" payload: ");
    Serial.println(payload);
    lastStreamId = entryId;
    handlePayload(payload);
  } else if (redis.lastError().length()) {
    dropRedis(F("xread"));
  }
  yield();
}

/** Periodically refreshes the `room:{id}:online` key so ops can detect outages. */
void maintainHeartbeat(unsigned long now) {
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if ((now - lastHeartbeatMs) < RECEIVER_HEARTBEAT_MS) {
    return;
  }
  if (!redis.setHeartbeat(contracts::key_online(roomId), contracts::kHeartbeatTtlSec)) {
    dropRedis(F("heartbeat"));
    return;
  }
  lastHeartbeatMs = now;
}

/** Returns true once SNTP has provided a modern epoch. */
bool timeIsValid() {
  return time(nullptr) >= kMinValidEpoch;
}

/** Copies the current localtime into the provided tm struct. */
bool acquireLocalTime(tm &out) {
  time_t now = time(nullptr);
  if (now < kMinValidEpoch) {
    return false;
  }
  localtime_r(&now, &out);
  return true;
}

/** Schedules SNTP sync attempts and logs when the clock locks in. */
void ensureClockSync(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!timeConfigured || (!timeIsValid() && (now - lastTimeSyncAttemptMs) > 10000)) {
    configTime(TZ_OFFSET_SECONDS, DST_OFFSET_SECONDS, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY,
               NTP_SERVER_TERTIARY);
    timeConfigured = true;
    lastTimeSyncAttemptMs = now;
    Serial.println(F("[time] requested SNTP sync"));
  }
  if (timeIsValid() && !timeAnnounced) {
    Serial.println(F("[time] clock synchronized"));
    timeAnnounced = true;
  }
}

/** Converts bounded hour/minute inputs into total minutes since midnight. */
uint16_t minutesFromClock(int hour, int minute) {
  if (hour < 0) {
    hour = 0;
  } else if (hour > 23) {
    hour = 23;
  }
  if (minute < 0) {
    minute = 0;
  } else if (minute > 59) {
    minute = 59;
  }
  return static_cast<uint16_t>(hour * 60 + minute);
}

/**
 * Loads the quiet-hour schedule from Redis and records whether it is active.
 */
bool fetchQuietHours() {
  if (!roomId.length()) {
    return false;
  }
  String payload;
  bool isNull = false;
  if (!redis.get(contracts::key_cfg(roomId), payload, &isNull)) {
    dropRedis(F("get cfg"));
    return false;
  }
  QuietHoursWindow next;
  next.startMinutes =
      minutesFromClock(SCHEDULE_DEFAULT_NIGHT_HOUR, SCHEDULE_DEFAULT_NIGHT_MINUTE);
  next.endMinutes = minutesFromClock(SCHEDULE_DEFAULT_WAKE_HOUR, SCHEDULE_DEFAULT_WAKE_MINUTE);
  next.enabled = true;
  next.version = 0;
  if (!isNull && payload.length()) {
    StaticJsonDocument<384> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print(F("[quiet] cfg json error: "));
      Serial.println(err.c_str());
    } else {
      JsonVariantConst night = doc["night"];
      JsonVariantConst wake = doc["wake"];
      bool nightEnabled = night["enabled"] | true;
      bool wakeEnabled = wake["enabled"] | true;
      next.startMinutes =
          minutesFromClock(night["hour"] | SCHEDULE_DEFAULT_NIGHT_HOUR,
                           night["minute"] | SCHEDULE_DEFAULT_NIGHT_MINUTE);
      next.endMinutes = minutesFromClock(wake["hour"] | SCHEDULE_DEFAULT_WAKE_HOUR,
                                         wake["minute"] | SCHEDULE_DEFAULT_WAKE_MINUTE);
      next.enabled = nightEnabled && wakeEnabled;
      next.version = doc["version"] | doc["cfg_ver"] | 0;
    }
  }
  if (next.startMinutes == next.endMinutes) {
    next.enabled = false;
  }
  quietWindow = next;
  quietWindowLoaded = true;
  Serial.print(F("[quiet] window "));
  Serial.print(quietWindow.enabled ? F("enabled") : F("disabled"));
  Serial.print(F(" start="));
  Serial.print(quietWindow.startMinutes / 60);
  Serial.print(':');
  Serial.print(quietWindow.startMinutes % 60);
  Serial.print(F(" end="));
  Serial.print(quietWindow.endMinutes / 60);
  Serial.print(':');
  Serial.println(quietWindow.endMinutes % 60);
  return true;
}

/** Refreshes the quiet-hours schedule based on a timer. */
void maybeRefreshQuietHours(unsigned long now) {
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if (quietWindowLoaded && (now - lastQuietFetchMs) < kQuietConfigRefreshMs) {
    return;
  }
  if (fetchQuietHours()) {
    lastQuietFetchMs = now;
  }
}

/** Returns true when the current time falls inside the configured quiet window. */
bool quietHoursActive(const tm &localNow) {
  if (!quietWindowLoaded || !quietWindow.enabled) {
    return false;
  }
  uint16_t minutes =
      static_cast<uint16_t>(((localNow.tm_hour % 24) * 60) + (localNow.tm_min % 60));
  uint16_t start = quietWindow.startMinutes % kMinutesPerDay;
  uint16_t end = quietWindow.endMinutes % kMinutesPerDay;
  if (start == end) {
    return false;
  }
  if (start < end) {
    return minutes >= start && minutes < end;
  }
  return minutes >= start || minutes < end;
}

/**
 * Samples the analog sound sensor and converts it to an estimated dB value.
 */
float readSoundDecibels() {
  if (!kSoundSensorEnabled) {
    return 0.0f;
  }
  uint32_t accumulator = 0;
  for (uint8_t i = 0; i < kSoundSampleCount; ++i) {
    uint16_t reading = static_cast<uint16_t>(analogRead(RECEIVER_SOUND_SENSOR_PIN));
    accumulator += reading;
    delayMicroseconds(200);
  }
  float average = static_cast<float>(accumulator) / static_cast<float>(kSoundSampleCount);
  if (average < 0.0f) {
    average = 0.0f;
  } else if (average > kSoundAdcMax) {
    average = kSoundAdcMax;
  }
  float normalized = average / kSoundAdcMax;
  float decibels = kSoundMinDb + normalized * (kSoundMaxDb - kSoundMinDb);
  if (decibels < kSoundMinDb) {
    decibels = kSoundMinDb;
  } else if (decibels > kSoundMaxDb) {
    decibels = kSoundMaxDb;
  }
  return decibels;
}

/**
 * Serializes the quiet-hour warning payload and stores it in Redis.
 */
bool publishSoundWarning(float decibels, uint32_t capturedAt) {
  if (!redis.connected() || !roomId.length()) {
    return false;
  }
  StaticJsonDocument<192> doc;
  doc["room"] = roomId;
  doc["decibels"] = decibels;
  doc["threshold"] = kSoundThresholdDb;
  doc["captured_at"] = capturedAt;
  doc["quiet"] = true;
  doc["source"] = F("receiver");
  doc["quiet_start_min"] = quietWindow.startMinutes;
  doc["quiet_end_min"] = quietWindow.endMinutes;
  doc["cfg_ver"] = quietWindow.version;
  warningScratch.remove(0);
  if (serializeJson(doc, warningScratch) == 0) {
    return false;
  }
  if (!redis.set(contracts::key_latest_warning(roomId), warningScratch)) {
    dropRedis(F("set warning"));
    return false;
  }
  Serial.print(F("[sound] warning "));
  Serial.print(decibels, 1);
  Serial.println(F(" dB"));
  return true;
}

/**
 * Samples the sound sensor, checks quiet-hour state, and publishes warnings.
 */
void monitorSound(unsigned long now) {
  if (!kSoundSensorEnabled) {
    return;
  }
  if ((now - lastSoundSampleMs) < kSoundSampleIntervalMs) {
    return;
  }
  lastSoundSampleMs = now;
  float decibels = readSoundDecibels();
  Serial.print(F("[sound] sample "));
  Serial.print(decibels, 1);
  Serial.println(F(" dB"));
  if (!roomId.length() || !redis.connected() || !quietWindowLoaded || !quietWindow.enabled) {
    return;
  }
  tm localNow;
  if (!acquireLocalTime(localNow) || !quietHoursActive(localNow)) {
    return;
  }
  if (decibels < kSoundThresholdDb) {
    return;
  }
  time_t epoch = time(nullptr);
  if (epoch < kMinValidEpoch) {
    return;
  }
  if ((now - lastWarningPublishedMs) < kSoundWarningCooldownMs) {
    return;
  }
  if (publishSoundWarning(decibels, static_cast<uint32_t>(epoch))) {
    lastWarningPublishedMs = now;
  }
}

}  // namespace

/** Arduino setup entry point that initializes IO and connectivity. */
void setup() {
  Serial.begin(115200);
  delay(100);
  logInfo(F("boot"));
  analogWriteRange(PWMRANGE);
  for (const auto &channel : kLedChannels) {
    pinMode(channel.pin, OUTPUT);
    writeLedDuty(channel.pin, 0, kLedActiveLow);
  }
  if (kSoundSensorEnabled) {
    pinMode(RECEIVER_SOUND_SENSOR_PIN, INPUT);
  }
  if (kStatusLedControllable) {
    pinMode(static_cast<uint8_t>(kStatusLedPin), OUTPUT);
    setStatusLed(false);
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(200);
#ifdef WIFI_HOSTNAME
  WiFi.hostname(WIFI_HOSTNAME);
#endif
  deviceId = WiFi.macAddress();
  randomSeed(ESP.getChipId());
  jsonScratch.reserve(128);
  warningScratch.reserve(160);
}

/** Main firmware loop orchestrating Wi-Fi/Redis, PWM, and monitoring. */
void loop() {
  unsigned long now = millis();
  updateStatusLed(now);
  if (!ensureWifi()) {
    delay(25);
    return;
  }
  ensureClockSync(now);
  if (!ensureRedis()) {
    delay(25);
    return;
  }
  if (!roomId.length()) {
    provisionRoom();
  }
  if (roomId.length() && !hasDesired) {
    pullSnapshot();
  }
  maintainHeartbeat(now);
  announceRoom(false);
  maybeRefreshQuietHours(now);
  pumpStream();
  monitorSound(now);
}
