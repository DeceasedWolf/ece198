#pragma once

#include <Arduino.h>
#include <Client.h>
#include <cstring>
#include <initializer_list>
#include <pgmspace.h>

/**
 * Minimal RESP (Redis Serialization Protocol) helper tailored for this demo.
 * It intentionally avoids heap allocations inside tight loops and only
 * implements the commands we rely upon in the firmware.
 */
class RedisLink {
 public:
  explicit RedisLink(Client &client) : client_(client) {
    lineBuffer_.reserve(64);
  }

  bool connected() const { return client_.connected(); }
  void stop() { client_.stop(); }

  void setTimeout(uint16_t ms) {
    timeoutMs_ = ms;
#ifdef ARDUINO
    client_.setTimeout(ms);
#endif
  }

  bool auth(const char *password) {
    if (!password || !password[0]) {
      return true;
    }
    return sendSimpleStatus({"AUTH", password});
  }

  bool ping() { return sendSimpleStatus({"PING"}); }

  bool set(const String &key, const String &value) {
    return sendSimpleStatus({RedisArg("SET"), RedisArg(key), RedisArg(value)});
  }

  bool get(const String &key, String &out, bool *isNull = nullptr) {
    if (!sendCommand({RedisArg("GET"), RedisArg(key)})) {
      return false;
    }
    return readBulkString(out, isNull);
  }

  bool expire(const String &key, uint16_t ttlSec) {
    char ttl[6];
    snprintf(ttl, sizeof(ttl), "%u", ttlSec);
    return sendIntegerEqualsOne({RedisArg("EXPIRE"), RedisArg(key), RedisArg(ttl)});
  }

  bool evalRoomScript(const __FlashStringHelper *script,
                      const String &deviceId,
                      uint16_t baseId,
                      String &roomId) {
    char base[8];
    snprintf(base, sizeof(base), "%u", baseId);
    if (!sendCommand({RedisArg("EVAL"), RedisArg(script), RedisArg("0"), RedisArg(deviceId), RedisArg(base)})) {
      return false;
    }
    return readBulkString(roomId);
  }

  bool xaddJson(const String &stream, const String &payload) {
    return sendBulkOnly({RedisArg("XADD"), RedisArg(stream), RedisArg("*"), RedisArg("p"), RedisArg(payload)});
  }

  bool xtrimApprox(const String &stream, uint16_t maxLen) {
    char lenStr[8];
    snprintf(lenStr, sizeof(lenStr), "%u", maxLen);
    return sendIntegerConsume({RedisArg("XTRIM"), RedisArg(stream), RedisArg("MAXLEN"), RedisArg("~"), RedisArg(lenStr)});
  }

  bool xreadLatest(const String &stream,
                   uint16_t blockMs,
                   const String &sinceId,
                   String &entryId,
                   String &payload) {
    char block[8];
    snprintf(block, sizeof(block), "%u", blockMs);
    if (!sendCommand({RedisArg("XREAD"),
                      RedisArg("BLOCK"),
                      RedisArg(block),
                      RedisArg("COUNT"),
                      RedisArg("1"),
                      RedisArg("STREAMS"),
                      RedisArg(stream),
                      RedisArg(sinceId)})) {
      return false;
    }
    entryId.remove(0);
    return readXreadPayload(entryId, payload);
  }

  bool streamTailId(const String &stream, String &entryId) {
    if (!sendCommand({RedisArg("XREVRANGE"),
                      RedisArg(stream),
                      RedisArg("+"),
                      RedisArg("-"),
                      RedisArg("COUNT"),
                      RedisArg("1")})) {
      return false;
    }
    return readXrevrangeTail(entryId);
  }

  bool setHeartbeat(const String &key, uint16_t ttlSec) {
    char ttl[6];
    snprintf(ttl, sizeof(ttl), "%u", ttlSec);
    return sendSimpleStatus({RedisArg("SET"), RedisArg(key), RedisArg("1"), RedisArg("EX"), RedisArg(ttl)});
  }

  const String &lastError() const { return lastError_; }

 private:
  struct RedisArg {
    const uint8_t *data;
    size_t len;
    bool progmem;
    RedisArg(const char *c)
        : data(reinterpret_cast<const uint8_t *>(c)),
          len(strlen(c)),
          progmem(false) {}
    RedisArg(const String &s)
        : data(reinterpret_cast<const uint8_t *>(s.c_str())),
          len(s.length()),
          progmem(false) {}
    RedisArg(const __FlashStringHelper *fs) {
      data = reinterpret_cast<const uint8_t *>(fs);
      len = strlen_P(reinterpret_cast<const char *>(fs));
      progmem = true;
    }
  };

  Client &client_;
  String lineBuffer_;
  String lastError_;
  uint16_t timeoutMs_ = 1500;

  bool sendCommand(std::initializer_list<RedisArg> args) {
    lastError_.remove(0);
    if (!connected()) {
      lastError_ = F("redis disconnected");
      return false;
    }
    client_.print('*');
    client_.print(args.size());
    client_.print("\r\n");
    for (const auto &arg : args) {
      client_.print('$');
      client_.print(arg.len);
      client_.print("\r\n");
      if (arg.len) {
        if (arg.progmem) {
          for (size_t i = 0; i < arg.len; ++i) {
            uint8_t b = pgm_read_byte(arg.data + i);
            client_.write(b);
          }
        } else {
          client_.write(arg.data, arg.len);
        }
      }
      client_.print("\r\n");
    }
    client_.flush();
    return true;
  }

  bool sendSimpleStatus(std::initializer_list<RedisArg> args) {
    if (!sendCommand(args)) {
      return false;
    }
    return readSimpleStatus();
  }

  bool sendIntegerEqualsOne(std::initializer_list<RedisArg> args) {
    long value = 0;
    return sendIntegerCommand(args, &value) && value == 1;
  }

  bool sendIntegerConsume(std::initializer_list<RedisArg> args) {
    return sendIntegerCommand(args, nullptr);
  }

  bool sendIntegerCommand(std::initializer_list<RedisArg> args, long *out) {
    if (!sendCommand(args)) {
      return false;
    }
    long value = 0;
    if (!readInteger(value)) {
      return false;
    }
    if (out) {
      *out = value;
    }
    return true;
  }

  bool sendBulkOnly(std::initializer_list<RedisArg> args) {
    if (!sendCommand(args)) {
      return false;
    }
    String tmp;
    return readBulkString(tmp);
  }

  bool readSimpleStatus() {
    char type;
    if (!readType(type, lineBuffer_)) {
      return false;
    }
    if (type == '+') {
      return true;
    }
    if (type == '-') {
      lastError_ = lineBuffer_;
    }
    return false;
  }

  bool readInteger(long &value) {
    char type;
    if (!readType(type, lineBuffer_)) {
      return false;
    }
    if (type == ':') {
      value = lineBuffer_.toInt();
      return true;
    }
    if (type == '-') {
      lastError_ = lineBuffer_;
    }
    return false;
  }

  bool readBulkString(String &out, bool *isNull = nullptr) {
    char type;
    if (!readType(type, lineBuffer_)) {
      return false;
    }
    if (type == '$') {
      long len = lineBuffer_.toInt();
      if (len < 0) {
        if (isNull) {
          *isNull = true;
        }
        out = "";
        return true;
      }
      out = "";
      out.reserve(len);
      while (len > 0) {
        char chunk[32];
        size_t chunkLen = len > 32 ? 32 : len;
        size_t read = client_.readBytes(chunk, chunkLen);
        if (read != chunkLen) {
          lastError_ = F("bulk read timeout");
          return false;
        }
        out.concat(chunk, read);
        len -= read;
      }
      if (!consumeCrlf()) {
        lastError_ = F("bulk missing CRLF");
        return false;
      }
      if (isNull) {
        *isNull = false;
      }
      return true;
    }
    if (type == '-') {
      lastError_ = lineBuffer_;
    }
    return false;
  }

  bool readArrayLen(int &len) {
    char type;
    if (!readType(type, lineBuffer_)) {
      return false;
    }
    if (type == '*') {
      len = lineBuffer_.toInt();
      return true;
    }
    if (type == '-') {
      lastError_ = lineBuffer_;
    }
    return false;
  }

  bool readType(char &type, String &line) {
    if (!connected()) {
      lastError_ = F("redis disconnected");
      return false;
    }
    unsigned long start = millis();
    while (!client_.available()) {
      if (millis() - start > timeoutMs_) {
        lastError_ = F("redis timeout");
        return false;
      }
      delay(1);
    }
    int c = client_.read();
    if (c < 0) {
      lastError_ = F("redis read err");
      return false;
    }
    type = static_cast<char>(c);
    line = client_.readStringUntil('\n');
    if (!line.length() && !client_.connected()) {
      lastError_ = F("redis closed");
      return false;
    }
    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }
    return true;
  }

  bool consumeCrlf() {
    char buf[2];
    size_t got = client_.readBytes(buf, sizeof(buf));
    return got == sizeof(buf) && buf[0] == '\r' && buf[1] == '\n';
  }

  bool readXreadPayload(String &entryIdOut, String &payload) {
    int topCount = 0;
    if (!readArrayLen(topCount)) {
      // Null reply => timeout, nothing to do.
      return false;
    }
    if (topCount <= 0) {
      return false;
    }
    for (int i = 0; i < topCount; ++i) {
      int pairLen = 0;
      if (!readArrayLen(pairLen) || pairLen < 2) {
        return false;
      }
      String streamName;
      if (!readBulkString(streamName)) {
        return false;
      }
      int entryCount = 0;
      if (!readArrayLen(entryCount) || entryCount <= 0) {
        continue;
      }
      for (int entry = 0; entry < entryCount; ++entry) {
        int entryLen = 0;
        if (!readArrayLen(entryLen) || entryLen < 2) {
          return false;
        }
        String entryId;
        if (!readBulkString(entryId)) {
          return false;
        }
        int fieldCount = 0;
        if (!readArrayLen(fieldCount) || fieldCount <= 0) {
          continue;
        }
        for (int field = 0; field < fieldCount; field += 2) {
          String fieldName;
          if (!readBulkString(fieldName)) {
            return false;
          }
          String fieldValue;
          if (!readBulkString(fieldValue)) {
            return false;
          }
          if (fieldName == "p") {
            payload = fieldValue;
            entryIdOut = entryId;
            return true;
          }
        }
      }
    }
    return false;
  }

  bool readXrevrangeTail(String &entryId) {
    int entryCount = 0;
    if (!readArrayLen(entryCount)) {
      return false;
    }
    if (entryCount <= 0) {
      entryId.remove(0);
      return true;
    }
    for (int i = 0; i < entryCount; ++i) {
      int entryLen = 0;
      if (!readArrayLen(entryLen) || entryLen < 2) {
        return false;
      }
      if (!readBulkString(entryId)) {
        return false;
      }
      int fieldCount = 0;
      if (!readArrayLen(fieldCount)) {
        return false;
      }
      String tmp;
      for (int field = 0; field < fieldCount; ++field) {
        if (!readBulkString(tmp)) {
          return false;
        }
      }
      break;
    }
    return true;
  }
};
