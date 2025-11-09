#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <cstring>
#include <time.h>

#include "config.h"
#include "contracts.hpp"
#include "redis_link.hpp"

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
contracts::Desired lastDesired;

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
}

void dropRedis(const __FlashStringHelper *context) {
  logRedisFailure(context);
  redis.stop();
  resetState();
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiBackoff.reset();
    return true;
  }
  unsigned long now = millis();
  if (!wifiBackoff.ready(now)) {
    return false;
  }
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiBackoff.schedule(now);
  return false;
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
#if defined(ROOM_ID_OVERRIDE) && (sizeof(ROOM_ID_OVERRIDE) > 1)
  if (!roomId.length()) {
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

uint8_t evaluateScheduleBrightness(const tm &now) {
  uint32_t seconds =
      static_cast<uint32_t>(now.tm_hour) * 3600UL + static_cast<uint32_t>(now.tm_min) * 60UL +
      static_cast<uint32_t>(now.tm_sec);
  uint8_t brightness = scheduleCfg.baselineBrightness;

  if (scheduleCfg.wakeEnabled) {
    uint32_t sunriseStart = static_cast<uint32_t>(scheduleCfg.wakeStartMin) * 60UL;
    uint32_t sunriseDuration = static_cast<uint32_t>(scheduleCfg.wakeDurationMin) * 60UL;
    if (sunriseDuration == 0) {
      if (seconds >= sunriseStart) {
        brightness = scheduleCfg.wakePeakBrightness;
      }
    } else if (seconds >= sunriseStart && seconds < sunriseStart + sunriseDuration) {
      uint32_t elapsed = seconds - sunriseStart;
      uint32_t scaled = static_cast<uint32_t>(scheduleCfg.wakePeakBrightness) * elapsed +
                        (sunriseDuration / 2);
      uint8_t ramp = static_cast<uint8_t>(scaled / sunriseDuration);
      if (ramp < scheduleCfg.baselineBrightness) {
        ramp = scheduleCfg.baselineBrightness;
      }
      brightness = ramp;
    } else if (seconds >= sunriseStart + sunriseDuration) {
      brightness = scheduleCfg.wakePeakBrightness;
    }
  }

  if (scheduleCfg.nightEnabled) {
    uint32_t nightStart = static_cast<uint32_t>(scheduleCfg.nightStartMin) * 60UL;
    uint32_t nightEnd =
        scheduleCfg.wakeEnabled ? static_cast<uint32_t>(scheduleCfg.wakeStartMin) * 60UL : nightStart;
    bool inNight = false;
    if (nightStart == nightEnd) {
      inNight = seconds >= nightStart;
    } else if (nightStart < nightEnd) {
      inNight = seconds >= nightStart && seconds < nightEnd;
    } else {
      inNight = seconds >= nightStart || seconds < nightEnd;
    }
    if (inNight) {
      brightness = scheduleCfg.nightBrightness;
    }
  }
  return clampPercent(brightness);
}

void maybePublishScheduledState(unsigned long now) {
  if ((now - lastSchedulePublishMs) < kSchedulePublishIntervalMs) {
    return;
  }
  lastSchedulePublishMs = now;
  if (!roomId.length() || !redis.connected() || !scheduleLoaded) {
    return;
  }
  tm localNow;
  if (!acquireLocalTime(localNow)) {
    return;
  }
  if (needsVersionSeed && !seedVersionFromRedis()) {
    return;
  }
  contracts::Desired desired = lastDesired;
  desired.brightness = evaluateScheduleBrightness(localNow);
  const char *mode = desired.brightness > 0 ? "on" : "off";
  if (!contracts::copyMode(mode, desired)) {
    return;
  }
  if (contracts::sameDesired(desired, lastDesired)) {
    return;
  }
  publishDesired(desired);
}

}  // namespace

void setup() {
  consoleSerial.begin(SENDER_CONSOLE_BAUD);
  log(F("boot"));
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
  ensureRoomFromOverride();
}

void loop() {
  unsigned long now = millis();
  pumpConsole();
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
  maybePublishScheduledState(now);
  maybeRequestRoom(now);
  yield();
}
