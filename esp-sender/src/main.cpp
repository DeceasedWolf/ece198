#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <cstring>
#include <cstdio>
#include <time.h>

#include "config.h"
#include "contracts.hpp"
#include "redis_link.hpp"

#ifndef SENDER_DISPLAY_ENABLED
#define SENDER_DISPLAY_ENABLED 1
#endif

#if SENDER_DISPLAY_ENABLED
#ifndef SENDER_DISPLAY_WIDTH
#define SENDER_DISPLAY_WIDTH 128
#endif
#ifndef SENDER_DISPLAY_HEIGHT
#define SENDER_DISPLAY_HEIGHT 64
#endif
#ifndef SENDER_DISPLAY_I2C_ADDRESS
#define SENDER_DISPLAY_I2C_ADDRESS 0x3C
#endif
#ifndef SENDER_DISPLAY_RESET_PIN
#define SENDER_DISPLAY_RESET_PIN -1
#endif
#ifndef SENDER_DISPLAY_SDA_PIN
#define SENDER_DISPLAY_SDA_PIN D2
#endif
#ifndef SENDER_DISPLAY_SCL_PIN
#define SENDER_DISPLAY_SCL_PIN D1
#endif
#ifndef SENDER_DISPLAY_REFRESH_INTERVAL_MS
#define SENDER_DISPLAY_REFRESH_INTERVAL_MS 1000
#endif
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

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

#ifndef SOUND_WARNING_DISPLAY_MS
#define SOUND_WARNING_DISPLAY_MS 15000
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

#ifndef SENDER_STATUS_LED_PIN
#define SENDER_STATUS_LED_PIN LED_BUILTIN
#endif

#ifndef SENDER_STATUS_LED_ACTIVE_LOW
#define SENDER_STATUS_LED_ACTIVE_LOW 1
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
constexpr int8_t kStatusLedPin = SENDER_STATUS_LED_PIN;
constexpr bool kStatusLedActiveLow = SENDER_STATUS_LED_ACTIVE_LOW;
constexpr bool kStatusLedEnabled = (SENDER_STATUS_LED_PIN >= 0);
constexpr bool kStatusLedControllable = kStatusLedEnabled;
constexpr unsigned long kStatusLedBlinkIntervalMs = 400;

void dropRedis(const __FlashStringHelper *context);

#if SENDER_DISPLAY_ENABLED
constexpr uint8_t kDisplayWidth = SENDER_DISPLAY_WIDTH;
constexpr uint8_t kDisplayHeight = SENDER_DISPLAY_HEIGHT;
constexpr int8_t kDisplayResetPin = SENDER_DISPLAY_RESET_PIN;
constexpr uint8_t kDisplayI2cAddress = SENDER_DISPLAY_I2C_ADDRESS;
constexpr unsigned long kDisplayRefreshIntervalMs = SENDER_DISPLAY_REFRESH_INTERVAL_MS;
constexpr uint8_t kDisplaySdaPin = SENDER_DISPLAY_SDA_PIN;
constexpr uint8_t kDisplaySclPin = SENDER_DISPLAY_SCL_PIN;

Adafruit_SSD1306 senderDisplay(kDisplayWidth, kDisplayHeight, &Wire, kDisplayResetPin);
bool displayReady = false;
unsigned long lastDisplayRefreshMs = 0;
constexpr unsigned long kWarningOverlayDurationMs = SOUND_WARNING_DISPLAY_MS;
constexpr unsigned long kWarningRefreshIntervalMs = 2000;
constexpr size_t kWarningJsonCapacity = 320;
constexpr uint32_t kWarningFreshWindowSec = 90;
constexpr unsigned long kWarningTimeGateMs = 8000;

struct DisplayPayload {
  char current[12]{};
  char quietStart[12]{};
  char quietEnd[12]{};
  bool quietEnabled = false;
  bool timeValid = false;
  bool warningActive = false;
};

DisplayPayload lastDisplayPayload{};
struct SoundWarningState {
  uint32_t capturedAt = 0;
  float decibels = 0.0f;
};
SoundWarningState latestWarning;
unsigned long warningOverlayUntilMs = 0;
unsigned long lastWarningFetchMs = 0;
unsigned long warningFetchGateStartMs = 0;
bool warningFetchGateOpen = false;
bool warningBootstrapPending = true;
String warningFetchJson;
#endif

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
bool desiredForcePublish = false;

bool acquireLocalTime(tm &out);
bool timeIsValid();

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
enum class StatusLedMode { Off, Solid, Blink };
StatusLedMode statusLedMode = StatusLedMode::Off;
bool statusLedBlinkState = true;
int8_t statusLedAppliedState = -1;
unsigned long statusLedLastToggleMs = 0;

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

#if SENDER_DISPLAY_ENABLED
void writeTimePlaceholder(char *dst, size_t len) {
  if (!len) {
    return;
  }
  static constexpr char kPlaceholder[] = "--:-- --";
  std::snprintf(dst, len, "%s", kPlaceholder);
}

void formatMinutes12(uint16_t minutes, char *dst, size_t len) {
  if (!len) {
    return;
  }
  minutes %= kMinutesPerDay;
  uint8_t hour24 = minutes / 60;
  uint8_t minute = minutes % 60;
  uint8_t hour12 = hour24 % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  const char *suffix = hour24 >= 12 ? "PM" : "AM";
  std::snprintf(dst, len, "%02u:%02u %s", hour12, minute, suffix);
}

void formatCurrentTime(const tm &now, char *dst, size_t len) {
  uint16_t minutes = static_cast<uint16_t>(((now.tm_hour % 24) * 60) + now.tm_min);
  formatMinutes12(minutes, dst, len);
}

bool payloadChanged(const DisplayPayload &a, const DisplayPayload &b) {
  if (a.timeValid != b.timeValid || a.quietEnabled != b.quietEnabled ||
      a.warningActive != b.warningActive) {
    return true;
  }
  if (std::strcmp(a.current, b.current) != 0) {
    return true;
  }
  if (std::strcmp(a.quietStart, b.quietStart) != 0) {
    return true;
  }
  if (std::strcmp(a.quietEnd, b.quietEnd) != 0) {
    return true;
  }
  return false;
}

void renderDisplay(const DisplayPayload &payload) {
  if (!displayReady) {
    return;
  }
  if (payload.warningActive) {
    senderDisplay.clearDisplay();
    senderDisplay.setTextColor(SSD1306_WHITE);
    senderDisplay.setTextSize(1);
    senderDisplay.setCursor(0, 26);
    senderDisplay.println(F("Sound Levels Exceeded"));
    senderDisplay.display();
    return;
  }
  senderDisplay.clearDisplay();
  senderDisplay.setTextColor(SSD1306_WHITE);
  senderDisplay.setTextSize(2);
  senderDisplay.setCursor(0, 0);
  senderDisplay.println(payload.current);
  senderDisplay.setTextSize(1);
  senderDisplay.setCursor(0, 32);
  senderDisplay.println(F("Quiet Hours:"));
  senderDisplay.setCursor(0, 46);
  char range[32];
  std::snprintf(range, sizeof(range), "%s - %s", payload.quietStart, payload.quietEnd);
  senderDisplay.println(range);
  senderDisplay.display();
}

void composeDisplayPayload(DisplayPayload &payload) {
  unsigned long nowMs = millis();
  if (warningOverlayUntilMs > 0) {
    long remaining = static_cast<long>(warningOverlayUntilMs - nowMs);
    payload.warningActive = remaining > 0;
  } else {
    payload.warningActive = false;
  }
  tm localNow;
  payload.timeValid = acquireLocalTime(localNow);
  if (payload.timeValid) {
    formatCurrentTime(localNow, payload.current, sizeof(payload.current));
  } else {
    writeTimePlaceholder(payload.current, sizeof(payload.current));
  }
  payload.quietEnabled = scheduleCfg.nightEnabled && scheduleCfg.wakeEnabled;
  if (payload.quietEnabled) {
    formatMinutes12(scheduleCfg.nightStartMin, payload.quietStart, sizeof(payload.quietStart));
    formatMinutes12(scheduleCfg.wakeStartMin, payload.quietEnd, sizeof(payload.quietEnd));
  } else {
    writeTimePlaceholder(payload.quietStart, sizeof(payload.quietStart));
    writeTimePlaceholder(payload.quietEnd, sizeof(payload.quietEnd));
  }
}

void initDisplayHardware() {
  if (displayReady) {
    return;
  }
  Wire.begin(kDisplaySdaPin, kDisplaySclPin);
  if (!senderDisplay.begin(SSD1306_SWITCHCAPVCC, kDisplayI2cAddress)) {
    logSerial.println(F("[display] init failed"));
    return;
  }
  senderDisplay.clearDisplay();
  senderDisplay.setTextColor(SSD1306_WHITE);
  senderDisplay.setTextSize(1);
  senderDisplay.setCursor(0, 0);
  senderDisplay.println(F("Booting..."));
  senderDisplay.display();
  displayReady = true;
  lastDisplayPayload = DisplayPayload();
}

void maybeUpdateDisplay(unsigned long now) {
  if (!displayReady) {
    return;
  }
  if ((now - lastDisplayRefreshMs) < kDisplayRefreshIntervalMs) {
    return;
  }
  lastDisplayRefreshMs = now;
  DisplayPayload payload;
  composeDisplayPayload(payload);
  if (!payloadChanged(payload, lastDisplayPayload)) {
    return;
  }
  renderDisplay(payload);
  lastDisplayPayload = payload;
}

bool decodeWarningJson(const String &payload, SoundWarningState &out) {
  StaticJsonDocument<kWarningJsonCapacity> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    logSerial.print(F("[display] warning parse failed: "));
    logSerial.println(err.c_str());
    return false;
  }
  uint32_t captured = doc["captured_at"] | 0;
  float decibels = doc["decibels"] | 0.0f;
  if (captured == 0) {
    return false;
  }
  out.capturedAt = captured;
  out.decibels = decibels;
  return true;
}

bool warningIsFresh(const SoundWarningState &state) {
  if (!timeIsValid()) {
    return true;
  }
  time_t nowEpoch = time(nullptr);
  if (nowEpoch < static_cast<time_t>(state.capturedAt)) {
    return true;
  }
  uint32_t age = static_cast<uint32_t>(nowEpoch - state.capturedAt);
  return age <= kWarningFreshWindowSec;
}

void maybeFetchLatestWarning(unsigned long now) {
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if (!warningFetchGateOpen) {
    if (warningFetchGateStartMs == 0) {
      warningFetchGateStartMs = now;
    }
    bool timeReady = timeIsValid();
    if (timeReady || (now - warningFetchGateStartMs) >= kWarningTimeGateMs) {
      warningFetchGateOpen = true;
    } else {
      return;
    }
  }
  if ((now - lastWarningFetchMs) < kWarningRefreshIntervalMs) {
    return;
  }
  lastWarningFetchMs = now;
  bool isNull = false;
  if (!redis.get(contracts::key_latest_warning(roomId), warningFetchJson, &isNull)) {
    dropRedis(F("get warning"));
    return;
  }
  if (isNull || !warningFetchJson.length()) {
    if (warningBootstrapPending) {
      warningBootstrapPending = false;
    }
    return;
  }
  SoundWarningState next;
  if (!decodeWarningJson(warningFetchJson, next)) {
    return;
  }
  if (next.capturedAt <= latestWarning.capturedAt) {
    return;
  }
  bool fresh = warningIsFresh(next);
  latestWarning = next;
  if (warningBootstrapPending && !fresh) {
    warningBootstrapPending = false;
    return;
  }
  warningBootstrapPending = false;
  if (!fresh) {
    return;
  }
  warningOverlayUntilMs = now + kWarningOverlayDurationMs;
  logSerial.printf("[display] sound warning %.1f dB\n", latestWarning.decibels);
  lastDisplayRefreshMs = 0;
}
#endif

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

void setStatusLed(bool on) {
  if (!kStatusLedControllable) {
    return;
  }
  int8_t target = on ? 1 : 0;
  if (statusLedAppliedState == target) {
    return;
  }
  statusLedAppliedState = target;
  bool levelHigh = on ? !kStatusLedActiveLow : kStatusLedActiveLow;
  digitalWrite(static_cast<uint8_t>(kStatusLedPin), levelHigh ? HIGH : LOW);
}

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

void resetState() {
  localVer = 0;
  needsVersionSeed = true;
  scheduleLoaded = false;
  lastScheduleFetchMs = 0;
  lastSchedulePublishMs = 0;
  overrideMirror = OverrideMirror();
  overrideDirty = false;
  lastOverrideFetchMs = 0;
#if SENDER_DISPLAY_ENABLED
  latestWarning = SoundWarningState();
  warningOverlayUntilMs = 0;
  lastWarningFetchMs = 0;
  warningFetchGateStartMs = 0;
  warningFetchGateOpen = false;
  warningBootstrapPending = true;
#endif
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
    resetState();
  }
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
  desiredForcePublish = true;
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
  auto decodeSnapshot = [](const String &payload, contracts::Desired &out) {
    if (!payload.length()) {
      return false;
    }
    StaticJsonDocument<contracts::kDesiredJsonCapacity> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      return false;
    }
    const char *mode = doc["mode"] | nullptr;
    if (mode && (strcmp(mode, "on") == 0 || strcmp(mode, "off") == 0)) {
      contracts::copyMode(mode, out);
    }
    out.brightness = doc["brightness"] | out.brightness;
    contracts::clampBrightness(out);
    out.ver = doc["ver"] | out.ver;
    return true;
  };
  auto loadSnapshot = [&](const String &key, const __FlashStringHelper *ctx, contracts::Desired &out) {
    bool isNull = false;
    String payload;
    if (!redis.get(key, payload, &isNull)) {
      dropRedis(ctx);
      return -1;
    }
    if (isNull || !payload.length()) {
      return 0;
    }
    if (!decodeSnapshot(payload, out)) {
      logSerial.print(F("[sender] ignored invalid desired snapshot from "));
      logSerial.println(key);
      return 0;
    }
    return 1;
  };
  contracts::Desired snapshotDesired;
  String desiredKey = contracts::key_desired(roomId);
  int desiredResult = loadSnapshot(desiredKey, F("seed desired get"), snapshotDesired);
  bool seededFromReported = false;
  if (desiredResult < 0) {
    return false;
  }
  if (desiredResult == 0) {
    String reportedKey = contracts::key_reported(roomId);
    int reportedResult = loadSnapshot(reportedKey, F("seed reported get"), snapshotDesired);
    if (reportedResult < 0) {
      return false;
    }
    seededFromReported = reportedResult == 1;
    if (reportedResult == 0) {
      snapshotDesired = contracts::Desired();
    }
  }
  localVer = snapshotDesired.ver;
  lastDesired = snapshotDesired;
  needsVersionSeed = false;
  if (localVer == 0) {
    logSerial.println(F("[sender] desired seed missing, starting at ver 0"));
  } else {
    logSerial.print(F("[sender] desired seed v="));
    logSerial.print(localVer);
    if (seededFromReported) {
      logSerial.println(F(" (reported)"));
    } else {
      logSerial.println(F(" (desired)"));
    }
  }
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
  uint32_t sunriseTarget = 0;
  uint32_t sunriseStart = 0;
  uint32_t sunriseDuration = 0;
  bool sunriseHasRamp = false;

  if (scheduleCfg.wakeEnabled) {
    sunriseTarget = static_cast<uint32_t>(scheduleCfg.wakeStartMin) * 60UL;
    uint16_t leadMinutes =
        scheduleCfg.wakeDurationMin > 0 ? scheduleCfg.wakeDurationMin : kWakeLeadMinutes;
    if (leadMinutes < kWakeLeadMinutes) {
      leadMinutes = kWakeLeadMinutes;
    }
    sunriseDuration = static_cast<uint32_t>(leadMinutes) * 60UL;
    sunriseStart = wrapSubtract(sunriseTarget, sunriseDuration);
    sunriseHasRamp = sunriseDuration > 0;
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
    uint32_t nightEnd = nightStart;
    if (scheduleCfg.wakeEnabled) {
      nightEnd = sunriseHasRamp ? sunriseStart : sunriseTarget;
    }
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
  bool same = contracts::sameDesired(desired, lastDesired);
  if (same && !desiredForcePublish) {
    overridePublishHint = false;
    lastSchedulePublishMs = now;
    return;
  }
  if (publishDesired(desired)) {
    desiredForcePublish = false;
    overridePublishHint = false;
    lastSchedulePublishMs = now;
  }
}

}  // namespace

void setup() {
  consoleSerial.begin(SENDER_CONSOLE_BAUD);
  log(F("boot"));
  initOverrideHardware();
#if SENDER_DISPLAY_ENABLED
  initDisplayHardware();
#endif
  if (kStatusLedControllable) {
    pinMode(static_cast<uint8_t>(kStatusLedPin), OUTPUT);
    setStatusLed(false);
  }
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
#if SENDER_DISPLAY_ENABLED
  warningFetchJson.reserve(160);
#endif
  ensureRoomFromOverride();
}

void loop() {
  unsigned long now = millis();
  updateStatusLed(now);
  pumpConsole();
  pumpOverrideInputs(now);
#if SENDER_DISPLAY_ENABLED
  maybeUpdateDisplay(now);
#endif
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
#if SENDER_DISPLAY_ENABLED
  maybeFetchLatestWarning(now);
#endif
  maybeFetchOverrideState(now);
  maybePublishOverrideState();
  maybePublishScheduledState(now);
  maybeRequestRoom(now);
  yield();
}
