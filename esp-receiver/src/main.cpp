#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

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
#ifndef RECEIVER_STATUS_LED_PIN
#define RECEIVER_STATUS_LED_PIN LED_BUILTIN
#endif
#ifndef RECEIVER_STATUS_LED_ACTIVE_LOW
#define RECEIVER_STATUS_LED_ACTIVE_LOW 1
#endif

constexpr uint8_t kLedPin = RECEIVER_LED_PIN;
constexpr bool kLedActiveLow = RECEIVER_LED_ACTIVE_LOW;
constexpr int8_t kStatusLedPin = RECEIVER_STATUS_LED_PIN;
constexpr bool kStatusLedActiveLow = RECEIVER_STATUS_LED_ACTIVE_LOW;
constexpr bool kStatusLedEnabled = (RECEIVER_STATUS_LED_PIN >= 0);
constexpr bool kStatusLedSharesDriver = kStatusLedEnabled && (RECEIVER_STATUS_LED_PIN == RECEIVER_LED_PIN);
constexpr bool kStatusLedShouldMirror = kStatusLedEnabled && !kStatusLedSharesDriver;

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

void logInfo(const __FlashStringHelper *msg) {
  Serial.print(F("[receiver] "));
  Serial.println(msg);
}

void logRedisFailure(const __FlashStringHelper *where) {
  Serial.print(F("[redis] "));
  Serial.print(where);
  Serial.print(F(": "));
  Serial.println(redis.lastError());
}

uint16_t applyPolarity(uint16_t duty, bool activeLow) {
  return activeLow ? (PWMRANGE - duty) : duty;
}

void writeLedDuty(uint8_t pin, uint16_t duty, bool activeLow) {
  analogWrite(pin, applyPolarity(duty, activeLow));
}

void resetRoomState(bool dropRoomId = true) {
  hasDesired = false;
  lastAppliedVer = 0;
  lastHeartbeatMs = 0;
  lastAnnounceMs = 0;
  streamCursorValid = false;
  lastStreamId.remove(0);
  if (dropRoomId) {
    roomId.remove(0);
  }
}

void dropRedis(const __FlashStringHelper *context) {
  logRedisFailure(context);
  redis.stop();
  resetRoomState();
}

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

void applyPwm(const contracts::Desired &desired) {
  uint16_t duty = 0;
  if (strcmp(desired.mode, "on") == 0 && desired.brightness > 0) {
    duty = map(desired.brightness, 0, 100, 0, PWMRANGE);
  }
  writeLedDuty(kLedPin, duty, kLedActiveLow);
  if (kStatusLedShouldMirror) {
    writeLedDuty(static_cast<uint8_t>(kStatusLedPin), duty, kStatusLedActiveLow);
  }
  Serial.printf("[pwm] pin=%u duty=%u mode=%s brightness=%u\n",
                static_cast<unsigned>(kLedPin),
                static_cast<unsigned>(duty),
                desired.mode,
                static_cast<unsigned>(desired.brightness));
}

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

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  logInfo(F("boot"));
  pinMode(kLedPin, OUTPUT);
  analogWriteRange(PWMRANGE);
  writeLedDuty(kLedPin, 0, kLedActiveLow);
  if (kStatusLedShouldMirror) {
    pinMode(static_cast<uint8_t>(kStatusLedPin), OUTPUT);
    writeLedDuty(static_cast<uint8_t>(kStatusLedPin), 0, kStatusLedActiveLow);
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
}

void loop() {
  unsigned long now = millis();
  if (!ensureWifi()) {
    delay(25);
    return;
  }
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
  pumpStream();
}
