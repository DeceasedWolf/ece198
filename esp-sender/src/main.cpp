#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <cstring>
#include <time.h>

#include "config.h"
#include "contracts.hpp"
#include "redis_link.hpp"

#ifndef PWMRANGE
#define PWMRANGE 1023
#endif

#ifndef SENDER_CONSOLE_BAUD
#define SENDER_CONSOLE_BAUD 115200
#endif

#ifndef SCHEDULE_REFRESH_MS
#define SCHEDULE_REFRESH_MS 30000
#endif

#ifndef SCHEDULE_PUBLISH_MIN_INTERVAL_MS
#define SCHEDULE_PUBLISH_MIN_INTERVAL_MS 1000
#endif

#ifndef SCHEDULE_DEFAULT_WAKE_HOUR
#define SCHEDULE_DEFAULT_WAKE_HOUR 7
#endif

#ifndef SCHEDULE_DEFAULT_WAKE_MINUTE
#define SCHEDULE_DEFAULT_WAKE_MINUTE 0
#endif

#ifndef SCHEDULE_DEFAULT_WAKE_DURATION_MIN
#define SCHEDULE_DEFAULT_WAKE_DURATION_MIN 20
#endif

#ifndef SCHEDULE_DEFAULT_WAKE_BRIGHTNESS
#define SCHEDULE_DEFAULT_WAKE_BRIGHTNESS 100
#endif

#ifndef SCHEDULE_DEFAULT_NIGHT_HOUR
#define SCHEDULE_DEFAULT_NIGHT_HOUR 22
#endif

#ifndef SCHEDULE_DEFAULT_NIGHT_MINUTE
#define SCHEDULE_DEFAULT_NIGHT_MINUTE 0
#endif

#ifndef SCHEDULE_DEFAULT_NIGHT_BRIGHTNESS
#define SCHEDULE_DEFAULT_NIGHT_BRIGHTNESS 5
#endif

#ifndef SCHEDULE_DEFAULT_BASELINE_BRIGHTNESS
#define SCHEDULE_DEFAULT_BASELINE_BRIGHTNESS 0
#endif

#ifndef QUIET_HOURS_DIM_MINUTES
#define QUIET_HOURS_DIM_MINUTES 90
#endif

#ifndef WAKE_BRIGHTEN_MINUTES
#define WAKE_BRIGHTEN_MINUTES 30
#endif

#ifndef OVERRIDE_POT_PIN
#define OVERRIDE_POT_PIN A0
#endif

#ifndef OVERRIDE_BUTTON_PIN
#define OVERRIDE_BUTTON_PIN D5
#endif

#ifndef OVERRIDE_BUTTON_PIN_MODE
#define OVERRIDE_BUTTON_PIN_MODE INPUT_PULLUP
#endif

#ifndef OVERRIDE_BUTTON_ACTIVE_LEVEL
#define OVERRIDE_BUTTON_ACTIVE_LEVEL LOW
#endif

#ifndef OVERRIDE_BUTTON_DEBOUNCE_MS
#define OVERRIDE_BUTTON_DEBOUNCE_MS 50
#endif

#ifndef OVERRIDE_ANALOG_MIN
#define OVERRIDE_ANALOG_MIN 0
#endif

#ifndef OVERRIDE_ANALOG_MAX
#define OVERRIDE_ANALOG_MAX 1023
#endif

#ifndef OVERRIDE_ANALOG_MIN_DELTA
#define OVERRIDE_ANALOG_MIN_DELTA 2
#endif

#ifndef TZ_OFFSET_SECONDS
#define TZ_OFFSET_SECONDS 0
#endif

#ifndef DST_OFFSET_SECONDS
#define DST_OFFSET_SECONDS 0
#endif

#ifndef NTP_SERVER_PRIMARY
#define NTP_SERVER_PRIMARY "pool.ntp.org"
#endif

#ifndef NTP_SERVER_SECONDARY
#define NTP_SERVER_SECONDARY "time.nist.gov"
#endif

#ifndef NTP_SERVER_TERTIARY
#define NTP_SERVER_TERTIARY "time.cloudflare.com"
#endif

namespace {

constexpr uint16_t kStreamTrimLen = 200;
constexpr uint16_t kRedisTimeoutMs = 1500;
constexpr unsigned long kRoomRequestIntervalMs = 1500;
constexpr size_t kConsoleMaxBytes = 128;
constexpr unsigned long kConsoleFlushMs = 500;
constexpr unsigned long kConfigRefreshIntervalMs = SCHEDULE_REFRESH_MS;
constexpr unsigned long kSchedulePublishIntervalMs = SCHEDULE_PUBLISH_MIN_INTERVAL_MS;
constexpr time_t kMinValidEpoch = 1609459200;  // 2021-01-01
constexpr uint16_t kMinutesPerDay = 24 * 60;
constexpr uint32_t kSecondsPerDay = static_cast<uint32_t>(kMinutesPerDay) * 60UL;
constexpr uint16_t kQuietLeadMinutes = QUIET_HOURS_DIM_MINUTES;
constexpr uint16_t kWakeLeadMinutes = WAKE_BRIGHTEN_MINUTES;
constexpr uint16_t kOverrideAnalogMin = OVERRIDE_ANALOG_MIN;
constexpr uint16_t kOverrideAnalogMax = OVERRIDE_ANALOG_MAX;
constexpr uint8_t kOverrideAnalogMinDelta = OVERRIDE_ANALOG_MIN_DELTA;
constexpr unsigned long kOverrideButtonDebounceMs = OVERRIDE_BUTTON_DEBOUNCE_MS;
constexpr unsigned long kOverrideRefreshIntervalMs = 2000;

HardwareSerial &consoleSerial = Serial;
HardwareSerial &logSerial = Serial;

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
bool needsVersionSeed = true;
uint32_t localVer = 0;
String jsonScratch;
String overrideJsonScratch;
contracts::Desired lastDesired;
bool overridePublishHint = false;

struct OverrideState {
  bool enabled = false;
  bool buttonStable = false;
  bool buttonReading = false;
  unsigned long buttonLastChangeMs = 0;
  uint8_t brightness = 0;
  uint16_t lastAnalogRaw = 0;
};

OverrideState overrideState;

struct OverrideMirror {
  bool known = false;
  bool enabled = false;
  uint32_t version = 0;
};

OverrideMirror overrideMirror;
bool overrideDirty = false;
unsigned long lastOverrideFetchMs = 0;

struct RoomSchedule {
  bool wakeEnabled = true;
  uint16_t wakeStartMin = static_cast<uint16_t>(SCHEDULE_DEFAULT_WAKE_HOUR * 60 +
                                                SCHEDULE_DEFAULT_WAKE_MINUTE);
  uint16_t wakeDurationMin = SCHEDULE_DEFAULT_WAKE_DURATION_MIN;
  uint8_t wakePeakBrightness = SCHEDULE_DEFAULT_WAKE_BRIGHTNESS;
  bool nightEnabled = true;
  uint16_t nightStartMin = static_cast<uint16_t>(SCHEDULE_DEFAULT_NIGHT_HOUR * 60 +
                                                 SCHEDULE_DEFAULT_NIGHT_MINUTE);
  uint8_t nightBrightness = SCHEDULE_DEFAULT_NIGHT_BRIGHTNESS;
  uint8_t baselineBrightness = SCHEDULE_DEFAULT_BASELINE_BRIGHTNESS;
  uint32_t version = 0;
};

RoomSchedule scheduleCfg;
bool scheduleLoaded = false;
unsigned long lastScheduleFetchMs = 0;
unsigned long lastSchedulePublishMs = 0;
unsigned long lastRoomPromptMs = 0;
bool timeConfigured = false;
bool timeAnnounced = false;
unsigned long lastTimeSyncAttemptMs = 0;

char consoleBuffer[kConsoleMaxBytes]{};
size_t consoleLen = 0;
unsigned long consoleLastByteMs = 0;

void log(const __FlashStringHelper *msg) {
  logSerial.print(F("[sender] "));
  logSerial.println(msg);
}

void logRedisFailure(const __FlashStringHelper *where) {
  logSerial.print(F("[redis] "));
  logSerial.print(where);
  logSerial.print(F(": "));
  logSerial.println(redis.lastError());
}

void resetState() {
  localVer = 0;
  needsVersionSeed = true;
  scheduleLoaded = false;
  lastScheduleFetchMs = 0;
  lastSchedulePublishMs = 0;
  overrideMirror = OverrideMirror();
  overrideDirty = false;
  lastOverrideFetchMs = 0;
}

void dropRedis(const __FlashStringHelper *context) {
  logRedisFailure(context);
  redis.stop();
  resetState();
}

void connectWifiBlocking() {
  logSerial.print(F("[wifi] connecting to "));
  logSerial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    logSerial.print(F("[wifi] status="));
    logSerial.println(static_cast<int>(WiFi.status()));
    delay(500);
    if ((millis() - start) > 20000) {
      logSerial.println(F("[wifi] retrying connection"));
      WiFi.disconnect(false);
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }
  logSerial.print(F("[wifi] connected ip="));
  logSerial.print(WiFi.localIP());
  logSerial.print(F(" rssi="));
  logSerial.println(WiFi.RSSI());
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiBackoff.reset();
    return true;
  }
  connectWifiBlocking();
  wifiBackoff.reset();
  return WiFi.status() == WL_CONNECTED;
}

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
  if (!redisClient.connect(REDIS_HOST, REDIS_PORT)) {
    redisBackoff.schedule(now);
    return false;
  }
  redis.setTimeout(kRedisTimeoutMs);
  redisClient.setNoDelay(true);
  if (!redis.auth(REDIS_PASSWORD)) {
    redisBackoff.schedule(now);
    dropRedis(F("auth"));
    return false;
  }
  if (!redis.ping()) {
    redisBackoff.schedule(now);
    dropRedis(F("ping"));
    return false;
  }
  redisBackoff.reset();
  needsVersionSeed = true;
  return true;
}

void handleRoomAnnouncement(const char *payload) {
  String rid = payload;
  rid.trim();
  if (!rid.length()) {
    return;
  }
  if (rid != roomId) {
    logSerial.print(F("[sender] room -> "));
    logSerial.println(rid);
    roomId = rid;
  }
  resetState();
}

uint8_t clampPercent(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return static_cast<uint8_t>(value);
}

uint16_t minuteOfDay(int hour, int minute) {
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

uint16_t clampDurationMinutes(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > kMinutesPerDay) {
    return kMinutesPerDay;
  }
  return static_cast<uint16_t>(value);
}

bool overrideButtonPressedLevel(int level) {
  if (OVERRIDE_BUTTON_ACTIVE_LEVEL == LOW) {
    return level == LOW;
  }
  return level == HIGH;
}

void logOverrideState() {
  logSerial.print(F("[override] "));
  logSerial.print(overrideState.enabled ? F("enabled ") : F("disabled "));
  logSerial.print(F("brightness="));
  logSerial.print(overrideState.brightness);
  logSerial.println('%');
}

uint8_t analogToPercent(uint16_t raw) {
  uint16_t clamped = raw;
  if (clamped < kOverrideAnalogMin) {
    clamped = kOverrideAnalogMin;
  } else if (clamped > kOverrideAnalogMax) {
    clamped = kOverrideAnalogMax;
  }
  uint16_t span = kOverrideAnalogMax > kOverrideAnalogMin ? (kOverrideAnalogMax - kOverrideAnalogMin) : 1;
  uint32_t scaled =
      static_cast<uint32_t>(clamped - kOverrideAnalogMin) * 100UL + static_cast<uint32_t>(span / 2);
  return static_cast<uint8_t>(scaled / span);
}

void setOverrideEnabled(bool enabled, bool syncToRedis = true) {
  if (overrideState.enabled == enabled) {
    return;
  }
  overrideState.enabled = enabled;
  if (syncToRedis) {
    overrideDirty = true;
  }
  overridePublishHint = true;
  logOverrideState();
}

void toggleOverride() { setOverrideEnabled(!overrideState.enabled, true); }

void pollOverrideButton(unsigned long now) {
  bool pressed = overrideButtonPressedLevel(digitalRead(OVERRIDE_BUTTON_PIN));
  if (pressed != overrideState.buttonReading) {
    overrideState.buttonReading = pressed;
    overrideState.buttonLastChangeMs = now;
    return;
  }
  if (overrideState.buttonStable == pressed) {
    return;
  }
  if ((now - overrideState.buttonLastChangeMs) < kOverrideButtonDebounceMs) {
    return;
  }
  overrideState.buttonStable = pressed;
  if (pressed) {
    toggleOverride();
  }
}

void pollOverrideAnalog() {
  uint16_t raw = analogRead(OVERRIDE_POT_PIN);
  overrideState.lastAnalogRaw = raw;
  uint8_t percent = analogToPercent(raw);
  if (percent == overrideState.brightness) {
    return;
  }
  uint8_t prev = overrideState.brightness;
  uint8_t diff = percent > prev ? percent - prev : prev - percent;
  overrideState.brightness = percent;
  if (diff >= kOverrideAnalogMinDelta && overrideState.enabled) {
    overridePublishHint = true;
  }
}

void initOverrideHardware() {
  pinMode(OVERRIDE_BUTTON_PIN, OVERRIDE_BUTTON_PIN_MODE);
  overrideState.buttonStable = overrideButtonPressedLevel(digitalRead(OVERRIDE_BUTTON_PIN));
  overrideState.buttonReading = overrideState.buttonStable;
  overrideState.buttonLastChangeMs = millis();
  overrideState.lastAnalogRaw = analogRead(OVERRIDE_POT_PIN);
  overrideState.brightness = analogToPercent(overrideState.lastAnalogRaw);
  logOverrideState();
}

void pumpOverrideInputs(unsigned long now) {
  pollOverrideButton(now);
  pollOverrideAnalog();
}

bool decodeOverrideJson(const String &json, bool &enabled, uint32_t &version) {
  if (!json.length()) {
    return false;
  }
  StaticJsonDocument<160> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    return false;
  }
  JsonVariant enabledField = doc["enabled"];
  if (enabledField.isNull()) {
    return false;
  }
  enabled = enabledField.as<bool>();
  if (!doc["ver"].isNull()) {
    version = doc["ver"].as<uint32_t>();
  } else if (!doc["version"].isNull()) {
    version = doc["version"].as<uint32_t>();
  } else {
    version = 0;
  }
  return true;
}

bool timeIsValid();

uint32_t overrideTimestamp() {
  if (timeIsValid()) {
    time_t now = time(nullptr);
    if (now >= 0) {
      return static_cast<uint32_t>(now);
    }
  }
  return static_cast<uint32_t>(millis());
}

void maybeFetchOverrideState(unsigned long now) {
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if ((now - lastOverrideFetchMs) < kOverrideRefreshIntervalMs) {
    return;
  }
  bool isNull = false;
  String payload;
  if (!redis.get(contracts::key_override(roomId), payload, &isNull)) {
    dropRedis(F("override get"));
    return;
  }
  lastOverrideFetchMs = now;
  if (isNull || !payload.length()) {
    return;
  }
  bool enabled = false;
  uint32_t version = 0;
  if (!decodeOverrideJson(payload, enabled, version)) {
    logSerial.println(F("[override] ignored invalid payload"));
    return;
  }
  if (!overrideMirror.known || version > overrideMirror.version) {
    overrideMirror.known = true;
    overrideMirror.version = version;
    overrideMirror.enabled = enabled;
    setOverrideEnabled(enabled, false);
    logSerial.print(F("[override] remote -> "));
    logSerial.print(enabled ? F("enabled") : F("disabled"));
    logSerial.print(F(" v="));
    logSerial.println(version);
  }
}

void maybePublishOverrideState() {
  if (!overrideDirty || !roomId.length() || !redis.connected()) {
    return;
  }
  StaticJsonDocument<192> doc;
  doc["enabled"] = overrideState.enabled;
  uint32_t newVer = overrideMirror.known ? (overrideMirror.version + 1) : 1;
  doc["ver"] = newVer;
  doc["updated_at"] = overrideTimestamp();
  doc["source"] = "device";
  overrideJsonScratch.remove(0);
  size_t docSize = measureJson(doc);
  overrideJsonScratch.reserve(docSize + 8);
  if (serializeJson(doc, overrideJsonScratch) == 0) {
    logSerial.println(F("[override] failed to encode json"));
    return;
  }
  if (!redis.set(contracts::key_override(roomId), overrideJsonScratch)) {
    dropRedis(F("set override"));
    return;
  }
  overrideMirror.known = true;
  overrideMirror.version = newVer;
  overrideMirror.enabled = overrideState.enabled;
  overrideDirty = false;
  logSerial.print(F("[override] stored "));
  logSerial.print(overrideState.enabled ? F("enabled") : F("disabled"));
  logSerial.print(F(" v="));
  logSerial.println(newVer);
}

bool decodeScheduleJson(const String &json, RoomSchedule &out) {
  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    return false;
  }
  RoomSchedule next;
  JsonVariant baseline = doc["baseline"]["brightness"];
  if (!baseline.isNull()) {
    next.baselineBrightness = clampPercent(baseline.as<int>());
  } else if (!doc["baseline_brightness"].isNull()) {
    next.baselineBrightness = clampPercent(doc["baseline_brightness"].as<int>());
  } else if (!doc["default_brightness"].isNull()) {
    next.baselineBrightness = clampPercent(doc["default_brightness"].as<int>());
  }
  JsonObject wake = doc["wake"];
  if (!wake.isNull()) {
    next.wakeEnabled = wake["enabled"] | next.wakeEnabled;
    int wakeHour = wake["hour"] | (next.wakeStartMin / 60);
    int wakeMinute = wake["minute"] | (next.wakeStartMin % 60);
    next.wakeStartMin = minuteOfDay(wakeHour, wakeMinute);
    next.wakeDurationMin = clampDurationMinutes(wake["duration_min"] | next.wakeDurationMin);
    next.wakePeakBrightness = clampPercent(wake["brightness"] | next.wakePeakBrightness);
  }
  JsonObject night = doc["night"];
  if (!night.isNull()) {
    next.nightEnabled = night["enabled"] | next.nightEnabled;
    int nightHour = night["hour"] | (next.nightStartMin / 60);
    int nightMinute = night["minute"] | (next.nightStartMin % 60);
    next.nightStartMin = minuteOfDay(nightHour, nightMinute);
    next.nightBrightness = clampPercent(night["brightness"] | next.nightBrightness);
  }
  if (!doc["version"].isNull()) {
    next.version = doc["version"].as<uint32_t>();
  } else if (!doc["cfg_ver"].isNull()) {
    next.version = doc["cfg_ver"].as<uint32_t>();
  }
  out = next;
  return true;
}

void logScheduleSummary() {
  logSerial.print(F("[schedule] baseline="));
  logSerial.print(scheduleCfg.baselineBrightness);
  logSerial.print(F("% wake["));
  logSerial.print(scheduleCfg.wakeEnabled ? F("on") : F("off"));
  logSerial.print(F("] "));
  auto printClock = [&](uint16_t minutes) {
    uint8_t hour = minutes / 60;
    uint8_t minute = minutes % 60;
    if (hour < 10) {
      logSerial.print('0');
    }
    logSerial.print(hour);
    logSerial.print(':');
    if (minute < 10) {
      logSerial.print('0');
    }
    logSerial.print(minute);
  };
  printClock(scheduleCfg.wakeStartMin);
  logSerial.print(F(" +"));
  logSerial.print(scheduleCfg.wakeDurationMin);
  logSerial.print(F("m -> "));
  logSerial.print(scheduleCfg.wakePeakBrightness);
  logSerial.print(F("% night["));
  logSerial.print(scheduleCfg.nightEnabled ? F("on") : F("off"));
  logSerial.print(F("] "));
  printClock(scheduleCfg.nightStartMin);
  logSerial.print(F(" -> "));
  logSerial.print(scheduleCfg.nightBrightness);
  logSerial.print(F("% v="));
  logSerial.println(scheduleCfg.version);
}

bool fetchScheduleConfig() {
  if (!roomId.length()) {
    return false;
  }
  bool isNull = false;
  String payload;
  if (!redis.get(contracts::key_cfg(roomId), payload, &isNull)) {
    dropRedis(F("cfg get"));
    return false;
  }
  if (isNull || !payload.length()) {
    scheduleCfg = RoomSchedule();
    scheduleLoaded = true;
    logSerial.println(F("[schedule] using defaults"));
    logScheduleSummary();
    return true;
  }
  RoomSchedule parsed;
  if (!decodeScheduleJson(payload, parsed)) {
    logSerial.println(F("[schedule] invalid cfg json, ignoring"));
    return false;
  }
  scheduleCfg = parsed;
  scheduleLoaded = true;
  logSerial.println(F("[schedule] config updated"));
  logScheduleSummary();
  return true;
}

void flushConsoleBuffer() { consoleLen = 0; }

void handleConsoleLine(const char *line) {
  if (!line || !line[0]) {
    return;
  }
  if (strncmp(line, "ROOM:", 5) == 0) {
    handleRoomAnnouncement(line + 5);
    return;
  }
  if (strcmp(line, "CFG?") == 0) {
    if (scheduleLoaded) {
      logScheduleSummary();
    } else {
      logSerial.println(F("[schedule] not loaded"));
    }
    return;
  }
  if (strcmp(line, "REFRESH") == 0) {
    scheduleLoaded = false;
    lastScheduleFetchMs = 0;
    logSerial.println(F("[schedule] refresh requested"));
    return;
  }
  logSerial.print(F("[sender] unknown console cmd: "));
  logSerial.println(line);
}

void pumpConsole() {
  while (consoleSerial.available()) {
    char c = static_cast<char>(consoleSerial.read());
    consoleLastByteMs = millis();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      consoleBuffer[consoleLen] = '\0';
      handleConsoleLine(consoleBuffer);
      flushConsoleBuffer();
      continue;
    }
    if (consoleLen < kConsoleMaxBytes - 1) {
      consoleBuffer[consoleLen++] = c;
    } else {
      flushConsoleBuffer();
    }
  }
  unsigned long now = millis();
  if (consoleLen > 0 && (now - consoleLastByteMs) > kConsoleFlushMs) {
    flushConsoleBuffer();
  }
}

void maybeRequestRoom(unsigned long now) {
  if (roomId.length()) {
    return;
  }
  if ((now - lastRoomPromptMs) < kRoomRequestIntervalMs) {
    return;
  }
  consoleSerial.println(F("ROOM?"));
  lastRoomPromptMs = now;
}

bool seedVersionFromRedis() {
  if (!roomId.length()) {
    return false;
  }
  bool isNull = false;
  String snapshot;
  if (!redis.get(contracts::key_desired(roomId), snapshot, &isNull)) {
    dropRedis(F("seed get"));
    return false;
  }
  if (isNull || !snapshot.length()) {
    localVer = 0;
    needsVersionSeed = false;
    return true;
  }
  contracts::Desired desired;
  if (!contracts::decodeDesired(snapshot, desired)) {
    localVer = 0;
  } else {
    localVer = desired.ver;
  }
  needsVersionSeed = false;
  return true;
}

bool publishDesired(contracts::Desired &desired) {
  if (desired.ver <= localVer) {
    desired.ver = localVer + 1;
  }
  if (!contracts::encodeDesired(desired, &roomId, jsonScratch)) {
    return false;
  }
  if (!redis.set(contracts::key_desired(roomId), jsonScratch)) {
    dropRedis(F("set desired"));
    return false;
  }
  if (!redis.xaddJson(contracts::stream_cmd(roomId), jsonScratch)) {
    dropRedis(F("xadd cmd"));
    return false;
  }
  redis.xtrimApprox(contracts::stream_cmd(roomId), kStreamTrimLen);
  localVer = desired.ver;
  lastDesired = desired;
  return true;
}


void ensureRoomFromOverride() {
#if defined(ROOM_ID_OVERRIDE)
  if (!roomId.length() && ROOM_ID_OVERRIDE[0] != '\0') {
    roomId = F(ROOM_ID_OVERRIDE);
    logSerial.print(F("[sender] room override -> "));
    logSerial.println(roomId);
    resetState();
  }
#endif
}

bool timeIsValid() { return time(nullptr) >= kMinValidEpoch; }

bool acquireLocalTime(tm &out) {
  time_t now = time(nullptr);
  if (now < kMinValidEpoch) {
    return false;
  }
  localtime_r(&now, &out);
  return true;
}

void ensureClockSync(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!timeConfigured || (!timeIsValid() && (now - lastTimeSyncAttemptMs) > 10000)) {
    configTime(TZ_OFFSET_SECONDS, DST_OFFSET_SECONDS, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY,
               NTP_SERVER_TERTIARY);
    timeConfigured = true;
    lastTimeSyncAttemptMs = now;
    logSerial.println(F("[time] requested SNTP sync"));
  }
  if (timeIsValid() && !timeAnnounced) {
    logSerial.println(F("[time] clock synchronized"));
    timeAnnounced = true;
  }
}

void maybeRefreshSchedule(unsigned long now) {
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if (scheduleLoaded && (now - lastScheduleFetchMs) < kConfigRefreshIntervalMs) {
    return;
  }
  if (fetchScheduleConfig()) {
    lastScheduleFetchMs = now;
  }
}

uint32_t wrapSubtract(uint32_t value, uint32_t delta) {
  if (kSecondsPerDay == 0) {
    return value;
  }
  delta %= kSecondsPerDay;
  if (delta > value) {
    return kSecondsPerDay - (delta - value);
  }
  return value - delta;
}

bool inWindow(uint32_t start, uint32_t end, uint32_t value) {
  if (start == end) {
    return false;
  }
  if (start < end) {
    return value >= start && value < end;
  }
  return value >= start || value < end;
}

uint32_t elapsedSince(uint32_t start, uint32_t value) {
  if (value >= start) {
    return value - start;
  }
  return kSecondsPerDay - start + value;
}

uint8_t lerpBrightness(uint8_t from, uint8_t to, uint32_t elapsed, uint32_t duration) {
  if (duration == 0) {
    return to;
  }
  if (elapsed > duration) {
    elapsed = duration;
  }
  int16_t delta = static_cast<int16_t>(to) - static_cast<int16_t>(from);
  int32_t scaled = static_cast<int32_t>(delta) * static_cast<int32_t>(elapsed) +
                   static_cast<int32_t>(duration / 2);
  int32_t result = static_cast<int32_t>(from) + scaled / static_cast<int32_t>(duration);
  if (result < 0) {
    result = 0;
  } else if (result > 100) {
    result = 100;
  }
  return static_cast<uint8_t>(result);
}

uint8_t evaluateScheduleBrightness(const tm &now) {
  uint32_t seconds =
      static_cast<uint32_t>(now.tm_hour) * 3600UL + static_cast<uint32_t>(now.tm_min) * 60UL +
      static_cast<uint32_t>(now.tm_sec);
  uint8_t brightness = scheduleCfg.baselineBrightness;
  uint8_t dayBrightness = brightness;

  if (scheduleCfg.wakeEnabled) {
    uint32_t sunriseTarget = static_cast<uint32_t>(scheduleCfg.wakeStartMin) * 60UL;
    uint16_t leadMinutes =
        scheduleCfg.wakeDurationMin > 0 ? scheduleCfg.wakeDurationMin : kWakeLeadMinutes;
    if (leadMinutes < kWakeLeadMinutes) {
      leadMinutes = kWakeLeadMinutes;
    }
    uint32_t sunriseDuration = static_cast<uint32_t>(leadMinutes) * 60UL;
    uint32_t sunriseStart = wrapSubtract(sunriseTarget, sunriseDuration);
    if (sunriseDuration == 0) {
      if (seconds >= sunriseTarget) {
        brightness = scheduleCfg.wakePeakBrightness;
      }
    } else {
      bool rampActive = inWindow(sunriseStart, sunriseTarget, seconds);
      if (rampActive) {
        uint32_t elapsed = elapsedSince(sunriseStart, seconds);
        uint32_t scaled = static_cast<uint32_t>(scheduleCfg.wakePeakBrightness) * elapsed +
                          (sunriseDuration / 2);
        uint8_t ramp = static_cast<uint8_t>(scaled / sunriseDuration);
        if (ramp < scheduleCfg.baselineBrightness) {
          ramp = scheduleCfg.baselineBrightness;
        }
        brightness = ramp;
      } else if (inWindow(sunriseTarget, sunriseStart, seconds)) {
        brightness = scheduleCfg.wakePeakBrightness;
      }
    }
  }
  dayBrightness = brightness;

  if (scheduleCfg.nightEnabled) {
    uint32_t nightStart = static_cast<uint32_t>(scheduleCfg.nightStartMin) * 60UL;
    uint32_t nightEnd =
        scheduleCfg.wakeEnabled ? static_cast<uint32_t>(scheduleCfg.wakeStartMin) * 60UL : nightStart;
    uint32_t quietRampDuration = static_cast<uint32_t>(kQuietLeadMinutes) * 60UL;
    uint32_t quietRampStart = wrapSubtract(nightStart, quietRampDuration);
    bool inQuiet = false;
    if (nightStart == nightEnd) {
      inQuiet = seconds >= nightStart;
    } else {
      inQuiet = inWindow(nightStart, nightEnd, seconds);
    }
    if (inQuiet) {
      brightness = scheduleCfg.nightBrightness;
    } else if (quietRampDuration > 0 && inWindow(quietRampStart, nightStart, seconds)) {
      uint32_t elapsed = elapsedSince(quietRampStart, seconds);
      brightness =
          lerpBrightness(dayBrightness, scheduleCfg.nightBrightness, elapsed, quietRampDuration);
    }
  }
  return clampPercent(brightness);
}

void maybePublishScheduledState(unsigned long now) {
  bool urgent = overridePublishHint;
  if (!urgent && (now - lastSchedulePublishMs) < kSchedulePublishIntervalMs) {
    return;
  }
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if (!scheduleLoaded && !overrideState.enabled) {
    return;
  }
  tm localNow;
  bool haveTime = acquireLocalTime(localNow);
  if (!overrideState.enabled && !haveTime) {
    return;
  }
  if (needsVersionSeed && !seedVersionFromRedis()) {
    return;
  }
  contracts::Desired desired = lastDesired;
  if (overrideState.enabled) {
    desired.brightness = overrideState.brightness;
  } else {
    desired.brightness = evaluateScheduleBrightness(localNow);
  }
  const char *mode = desired.brightness > 0 ? "on" : "off";
  if (!contracts::copyMode(mode, desired)) {
    return;
  }
  if (contracts::sameDesired(desired, lastDesired)) {
    overridePublishHint = false;
    lastSchedulePublishMs = now;
    return;
  }
  if (publishDesired(desired)) {
    overridePublishHint = false;
    lastSchedulePublishMs = now;
  }
}

}  // namespace

void setup() {
  consoleSerial.begin(SENDER_CONSOLE_BAUD);
  log(F("boot"));
  initOverrideHardware();
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#ifdef WIFI_HOSTNAME
  {
    String host = String(WIFI_HOSTNAME) + "-sender";
    WiFi.hostname(host);
  }
#endif
  randomSeed(ESP.getChipId());
  jsonScratch.reserve(192);
  overrideJsonScratch.reserve(128);
  ensureRoomFromOverride();
}

void loop() {
  unsigned long now = millis();
  pumpConsole();
  pumpOverrideInputs(now);
  ensureRoomFromOverride();
  if (!ensureWifi()) {
    maybeRequestRoom(now);
    delay(10);
    return;
  }
  ensureClockSync(now);
  if (!ensureRedis()) {
    maybeRequestRoom(now);
    delay(10);
    return;
  }
  ensureRoomFromOverride();
  ensureClockSync(now);
  maybeRefreshSchedule(now);
  maybeFetchOverrideState(now);
  maybePublishOverrideState();
  maybePublishScheduledState(now);
  maybeRequestRoom(now);
  yield();
}
