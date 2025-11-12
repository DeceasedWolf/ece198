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

constexpr uint8_t kLedPin = RECEIVER_LED_PIN;

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
String lastStreamId("0-0");
unsigned long lastHeartbeatMs = 0;
unsigned long lastAnnounceMs = 0;
String jsonScratch;
bool wifiAnnounced = false;

bool replaceUtf8Quotes(String &text) {
  bool modified = false;
  String normalized;
  normalized.reserve(text.length());
  for (size_t i = 0; i < text.length(); ++i) {
    uint8_t b0 = static_cast<uint8_t>(text[i]);
    if (b0 == 0xE2 && (i + 2) < text.length()) {
      uint8_t b1 = static_cast<uint8_t>(text[i + 1]);
      uint8_t b2 = static_cast<uint8_t>(text[i + 2]);
      if (b1 == 0x80 && (b2 == 0x9C || b2 == 0x9D || b2 == 0x98 || b2 == 0x99)) {
        normalized += '"';
        modified = true;
        i += 2;
        continue;
      }
    }
    normalized += static_cast<char>(b0);
  }
  if (modified) {
    text = normalized;
  }
  return modified;
}

bool stripWrappingQuotes(String &text) {
  bool modified = false;
  while (text.length() >= 2 && text[0] == '"' && text[text.length() - 1] == '"') {
    text.remove(text.length() - 1);
    text.remove(0, 1);
    modified = true;
  }
  return modified;
}

bool isolateJsonObject(String &text) {
  int start = text.indexOf('{');
  int end = text.lastIndexOf('}');
  if (start < 0 || end < 0 || end <= start) {
    return false;
  }
  if (start == 0 && end == static_cast<int>(text.length()) - 1) {
    return false;
  }
  text = text.substring(start, end + 1);
  return true;
}

void dumpPayloadHex(const String &payload) {
  Serial.print(F("[stream] raw bytes:"));
  for (size_t i = 0; i < payload.length(); ++i) {
    uint8_t b = static_cast<uint8_t>(payload[i]);
    Serial.print(' ');
    if (b < 16) {
      Serial.print('0');
    }
    Serial.print(static_cast<unsigned>(b), HEX);
  }
  Serial.println();
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

void resetRoomState(bool dropRoomId = true) {
  hasDesired = false;
  lastAppliedVer = 0;
  lastHeartbeatMs = 0;
  lastAnnounceMs = 0;
  lastStreamId = "0-0";
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
#if RECEIVER_LED_ACTIVE_LOW
  duty = PWMRANGE - duty;
#endif
  analogWrite(kLedPin, duty);
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

void handlePayload(String payload) {
  bool modified = false;
  int originalLen = payload.length();
  payload.trim();
  if (payload.length() != originalLen) {
    modified = true;
  }
  if (replaceUtf8Quotes(payload)) {
    modified = true;
  }
  if (stripWrappingQuotes(payload)) {
    modified = true;
  }
  if (isolateJsonObject(payload)) {
    modified = true;
  }
  if (!payload.length()) {
    Serial.println(F("[stream] empty payload after sanitize"));
    return;
  }
  StaticJsonDocument<contracts::kDesiredJsonCapacity> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print(F("[stream] json error: "));
    Serial.println(err.c_str());
    dumpPayloadHex(payload);
    return;
  }
  const char *mode = nullptr;
  if (doc["mode"].is<const char *>()) {
    mode = doc["mode"];
  } else if (doc["mode"].is<String>()) {
    static String tmpMode;
    tmpMode = doc["mode"].as<String>();
    mode = tmpMode.c_str();
  }
  contracts::Desired desired = lastDesired;
  if (!contracts::copyMode(mode, desired)) {
    Serial.print(F("[stream] invalid mode: "));
    Serial.println(mode ? mode : "(null)");
    Serial.print(F("[stream] doc dump: "));
    serializeJson(doc, Serial);
    Serial.println();
    Serial.print(F("[stream] doc mem: "));
    Serial.println(doc.memoryUsage());
    dumpPayloadHex(payload);
    return;
  }
  desired.brightness = doc["brightness"] | desired.brightness;
  contracts::clampBrightness(desired);
  desired.ver = doc["ver"] | desired.ver;
  if (modified) {
    Serial.print(F("[stream] sanitized payload: "));
    Serial.println(payload);
  }
  if (desired.ver < lastAppliedVer) {
    Serial.print(F("[stream] stale ver "));
    Serial.print(desired.ver);
    Serial.print(F(" < "));
    Serial.println(lastAppliedVer);
    return;
  }
  if (!contracts::encodeDesired(desired, &roomId, jsonScratch)) {
    Serial.println(F("[stream] encode failed"));
    return;
  }
  applyPwm(desired);
  lastDesired = desired;
  lastAppliedVer = desired.ver;
  hasDesired = true;
  if (!recordState(jsonScratch)) {
    Serial.println(F("[stream] record failed"));
  }
}

void pumpStream() {
  if (!roomId.length()) {
    return;
  }
  String payload;
  String entryId;
  const String sinceId = lastStreamId.length() ? lastStreamId : String("0-0");
  if (redis.xreadLatest(contracts::stream_cmd(roomId), kXreadBlockMs, sinceId, entryId, payload)) {
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
  analogWrite(kLedPin, RECEIVER_LED_ACTIVE_LOW ? PWMRANGE : 0);
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
