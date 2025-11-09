#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <cstring>

#include "config.h"
#include "contracts.hpp"
#include "redis_link.hpp"

namespace {

constexpr uint16_t kStreamTrimLen = 200;
constexpr uint16_t kRedisTimeoutMs = 1500;
constexpr unsigned long kRoomRequestIntervalMs = 1500;

HardwareSerial &panelSerial = Serial;
HardwareSerial &logSerial = Serial1;

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

char panelBuffer[PANEL_FRAME_MAX_BYTES]{};
size_t panelLen = 0;
unsigned long panelLastByteMs = 0;
unsigned long lastRoomPromptMs = 0;

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

void flushPanelBuffer() { panelLen = 0; }

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

bool handlePanelJson(const char *line) {
  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    return false;
  }
  contracts::Desired desired = lastDesired;  // start from last applied
  const char *mode = doc["mode"] | nullptr;
  if (mode) {
    if (!contracts::copyMode(mode, desired)) {
      return false;
    }
  }
  if (!doc["brightness"].isNull()) {
    int value = doc["brightness"].as<int>();
    if (value < 0) {
      value = 0;
    } else if (value > 100) {
      value = 100;
    }
    desired.brightness = static_cast<uint8_t>(value);
  }
  if (doc["ver"].is<uint32_t>()) {
    desired.ver = doc["ver"].as<uint32_t>();
  }
  return publishDesired(desired);
}

void handlePanelLine(const char *line) {
  if (!line || !line[0]) {
    return;
  }
  if (strncmp(line, "ROOM:", 5) == 0) {
    handleRoomAnnouncement(line + 5);
    return;
  }
  if (!roomId.length() || !redis.connected()) {
    return;
  }
  if (needsVersionSeed && !seedVersionFromRedis()) {
    return;
  }
  handlePanelJson(line);
}

void pumpPanelSerial() {
  while (panelSerial.available()) {
    char c = static_cast<char>(panelSerial.read());
    panelLastByteMs = millis();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      panelBuffer[panelLen] = '\0';
      handlePanelLine(panelBuffer);
      flushPanelBuffer();
      continue;
    }
    if (panelLen < PANEL_FRAME_MAX_BYTES - 1) {
      panelBuffer[panelLen++] = c;
    } else {
      flushPanelBuffer();
    }
  }
  unsigned long now = millis();
  if (panelLen > 0 && (now - panelLastByteMs) > PANEL_FRAME_TIMEOUT_MS) {
    flushPanelBuffer();
  }
}

void maybeRequestRoom(unsigned long now) {
  if (roomId.length()) {
    return;
  }
  if ((now - lastRoomPromptMs) < kRoomRequestIntervalMs) {
    return;
  }
  panelSerial.println(F("ROOM?"));
  lastRoomPromptMs = now;
}

}  // namespace

void setup() {
  logSerial.begin(115200);
  panelSerial.begin(ROOM_LINK_BAUD);
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
}

void loop() {
  unsigned long now = millis();
  pumpPanelSerial();
  if (!ensureWifi()) {
    maybeRequestRoom(now);
    delay(10);
    return;
  }
  if (!ensureRedis()) {
    maybeRequestRoom(now);
    delay(10);
    return;
  }
  maybeRequestRoom(now);
  yield();
}
