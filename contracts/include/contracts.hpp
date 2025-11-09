#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace contracts {

constexpr uint32_t kHeartbeatTtlSec = 10;
constexpr size_t kDesiredJsonCapacity = 192;

struct Desired {
  char mode[4] = "off";
  uint8_t brightness = 0;
  uint32_t ver = 0;
};

inline String makeRoomKey(const String &roomId, const char *suffix) {
  String key("room:");
  key.reserve(roomId.length() + strlen(suffix) + 6);
  key += roomId;
  key += suffix;
  return key;
}

inline String key_cfg(const String &roomId) { return makeRoomKey(roomId, ":cfg"); }
inline String key_desired(const String &roomId) { return makeRoomKey(roomId, ":desired"); }
inline String key_reported(const String &roomId) { return makeRoomKey(roomId, ":reported"); }
inline String key_online(const String &roomId) { return makeRoomKey(roomId, ":online"); }

inline String stream_cmd(const String &roomId) {
  String key("cmd:room:");
  key.reserve(roomId.length() + 9);
  key += roomId;
  return key;
}

inline String stream_state(const String &roomId) {
  String key("state:room:");
  key.reserve(roomId.length() + 11);
  key += roomId;
  return key;
}

inline bool copyMode(const char *src, Desired &dst) {
  if (!src) {
    return false;
  }
  if (strcmp(src, "on") != 0 && strcmp(src, "off") != 0) {
    return false;
  }
  strncpy(dst.mode, src, sizeof(dst.mode) - 1);
  dst.mode[sizeof(dst.mode) - 1] = '\0';
  return true;
}

inline void clampBrightness(Desired &desired) {
  if (desired.brightness > 100) {
    desired.brightness = 100;
  }
}

inline bool decodeDesired(const String &json, Desired &out) {
  StaticJsonDocument<kDesiredJsonCapacity> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    return false;
  }
  const char *mode = doc["mode"] | nullptr;
  if (!copyMode(mode, out)) {
    return false;
  }
  out.brightness = doc["brightness"] | out.brightness;
  clampBrightness(out);
  out.ver = doc["ver"] | out.ver;
  return true;
}

inline bool encodeDesired(const Desired &desired, const String *roomId, String &out) {
  StaticJsonDocument<kDesiredJsonCapacity> doc;
  doc["mode"] = desired.mode;
  doc["brightness"] = desired.brightness;
  doc["ver"] = desired.ver;
  if (roomId && roomId->length() > 0) {
    doc["room"] = *roomId;
  }
  out.remove(0);
  size_t size = measureJson(doc);
  out.reserve(size + 8);
  return serializeJson(doc, out) > 0;
}

inline bool sameDesired(const Desired &lhs, const Desired &rhs) {
  return lhs.brightness == rhs.brightness && lhs.ver == rhs.ver && strcmp(lhs.mode, rhs.mode) == 0;
}

}  // namespace contracts
