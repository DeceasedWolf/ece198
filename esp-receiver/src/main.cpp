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

constexpr uint8_t kLedPin = D4;
constexpr uint16_t kStreamTrimLen = 200;
constexpr uint32_t kXreadBlockMs = 1000;
constexpr uint16_t kRedisTimeoutMs = 1500;

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
unsigned long lastHeartbeatMs = 0;
unsigned long lastAnnounceMs = 0;
String jsonScratch;
bool wifiAnnounced = false;
unsigned long lastWifiStatusLogMs = 0;

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

void resetRoomState(bool dropRoomId = true) {
  hasDesired = false;
  lastAppliedVer = 0;
  lastHeartbeatMs = 0;
  lastAnnounceMs = 0;
  if (dropRoomId) {
    roomId.remove(0);
  }
}

void dropRedis(const __FlashStringHelper *context) {
  logRedisFailure(context);
  redis.stop();
  resetRoomState();
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
  unsigned long now = millis();
  if (!wifiBackoff.ready(now)) {
    if ((now - lastWifiStatusLogMs) > 1000) {
      logWifiSnapshot(F("waiting"));
      lastWifiStatusLogMs = now;
    }
    return false;
  }
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logWifiSnapshot(F("begin"));
  lastWifiStatusLogMs = now;
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
  if (!redis.evalRoomScript(FPSTR(kProvisionScript), deviceId, PROVISIONING_BASE_ID, rid)) {
    dropRedis(F("provision"));
    return false;
  }
  if (!rid.length()) {
    return false;
  }
  if (rid != roomId) {
    roomId = rid;
    resetRoomState();
  }
  announceRoom(true);
  return true;
}

void applyPwm(const contracts::Desired &desired) {
  uint16_t duty = 0;
  if (strcmp(desired.mode, "on") == 0 && desired.brightness > 0) {
    duty = map(desired.brightness, 0, 100, 0, PWMRANGE);
  }
  analogWrite(kLedPin, duty);
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
  if (!contracts::decodeDesired(stored, desired)) {
    desired = contracts::Desired();
  }
  if (!contracts::encodeDesired(desired, &roomId, jsonScratch)) {
    return false;
  }
  applyPwm(desired);
  lastDesired = desired;
  lastAppliedVer = desired.ver;
  hasDesired = true;
  return recordState(jsonScratch);
}

void handlePayload(const String &payload) {
  contracts::Desired desired = lastDesired;
  if (!contracts::decodeDesired(payload, desired)) {
    return;
  }
  if (desired.ver < lastAppliedVer) {
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
  if (!roomId.length()) {
    return;
  }
  String payload;
  if (redis.xreadLatest(contracts::stream_cmd(roomId), kXreadBlockMs, payload)) {
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
  analogWrite(kLedPin, 0);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
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
