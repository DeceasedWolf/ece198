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
  /**
   * Constructs a RedisLink that will operate over the provided Client implementation.
   */
  explicit RedisLink(Client &client) : client_(client) {
    lineBuffer_.reserve(64);
  }

  /** Returns true when the underlying TCP client is still connected. */
  bool connected() const { return client_.connected(); }
  /** Immediately closes the underlying connection. */
  void stop() { client_.stop(); }

  /**
   * Updates the read timeout (milliseconds) used by blocking RESP operations.
   */
  void setTimeout(uint16_t ms) {
    timeoutMs_ = ms;
#ifdef ARDUINO
    client_.setTimeout(ms);
#endif
  }

  /**
   * Issues an `AUTH` command when a password is provided, returning true on success.
   */
  bool auth(const char *password) {
    if (!password || !password[0]) {
      return true;
    }
    return sendSimpleStatus({"AUTH", password});
  }

  /** Sends a `PING` round-trip to verify liveness. */
  bool ping() { return sendSimpleStatus({"PING"}); }

  /**
   * Executes `SET key value` to overwrite a Redis string.
   */
  bool set(const String &key, const String &value) {
    return sendSimpleStatus({RedisArg("SET"), RedisArg(key), RedisArg(value)});
  }

  /**
   * Executes `GET key` and stores the response. When `isNull` is supplied it reports nil replies.
   */
  bool get(const String &key, String &out, bool *isNull = nullptr) {
    if (!sendCommand({RedisArg("GET"), RedisArg(key)})) {
      return false;
    }
    return readBulkString(out, isNull);
  }

  /** Sets an expire TTL (seconds) for the given key. */
  bool expire(const String &key, uint16_t ttlSec) {
    char ttl[6];
    snprintf(ttl, sizeof(ttl), "%u", ttlSec);
    return sendIntegerEqualsOne({RedisArg("EXPIRE"), RedisArg(key), RedisArg(ttl)});
  }

  /**
   * Runs the provisioning Lua script and returns the allocated room id for the device.
   */
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

  /**
   * Appends a JSON payload to the provided stream with the field name `p`.
   */
  bool xaddJson(const String &stream, const String &payload) {
    return sendBulkOnly({RedisArg("XADD"), RedisArg(stream), RedisArg("*"), RedisArg("p"), RedisArg(payload)});
  }

  /**
   * Soft-trims a stream (`XTRIM MAXLEN ~`) to bound Redis memory usage.
   */
  bool xtrimApprox(const String &stream, uint16_t maxLen) {
    char lenStr[8];
    snprintf(lenStr, sizeof(lenStr), "%u", maxLen);
    return sendIntegerConsume({RedisArg("XTRIM"), RedisArg(stream), RedisArg("MAXLEN"), RedisArg("~"), RedisArg(lenStr)});
  }

  /**
   * Performs an `XREAD` from `stream`, blocking for up to `blockMs` until a new entry appears.
   * Stores the record id and payload when a command is delivered.
   */
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

  /**
   * Reads the latest stream entry id using `XREVRANGE` so consumers can resume at the tail.
   */
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

  /**
   * Writes a simple heartbeat key with an `EX` TTL so monitoring can detect offline devices.
   */
  bool setHeartbeat(const String &key, uint16_t ttlSec) {
    char ttl[6];
    snprintf(ttl, sizeof(ttl), "%u", ttlSec);
    return sendSimpleStatus({RedisArg("SET"), RedisArg(key), RedisArg("1"), RedisArg("EX"), RedisArg(ttl)});
  }

  /** Holds the last Redis protocol error string for debugging. */
  const String &lastError() const { return lastError_; }

 private:
  /**
   * Simple view into the argument bytes for RESP serialization.
   */
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

  /**
   * Serializes and writes a RESP command made up of the provided argument list.
   */
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

  /**
   * Sends a command that should return a simple status reply (e.g. `+OK`).
   */
  bool sendSimpleStatus(std::initializer_list<RedisArg> args) {
    if (!sendCommand(args)) {
      return false;
    }
    return readSimpleStatus();
  }

  /**
   * Sends a command that returns `:1` on success and checks the integer response.
   */
  bool sendIntegerEqualsOne(std::initializer_list<RedisArg> args) {
    long value = 0;
    return sendIntegerCommand(args, &value) && value == 1;
  }

  /**
   * Sends a command while discarding the integer response.
   */
  bool sendIntegerConsume(std::initializer_list<RedisArg> args) {
    return sendIntegerCommand(args, nullptr);
  }

  /**
   * Sends a command that returns an integer and optionally stores the parsed value.
   */
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

  /**
   * Issues a command and consumes the bulk-string response without keeping it.
   */
  bool sendBulkOnly(std::initializer_list<RedisArg> args) {
    if (!sendCommand(args)) {
      return false;
    }
    String tmp;
    return readBulkString(tmp);
  }

  /**
   * Reads a simple status line (`+OK` or `-ERR`) after issuing a command.
   */
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

  /**
   * Reads an integer reply (`:1234`) and stores the value when available.
   */
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

  /**
   * Reads a bulk-string reply into `out`, optionally reporting when the server returned nil.
   */
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

  /**
   * Parses an array length header (`*N`) from the stream.
   */
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

  /**
   * Reads the RESP type byte and line payload, handling connection errors/timeouts.
   */
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

  /** Consumes the CRLF that terminates bulk-string payloads. */
  bool consumeCrlf() {
    char buf[2];
    size_t got = client_.readBytes(buf, sizeof(buf));
    return got == sizeof(buf) && buf[0] == '\r' && buf[1] == '\n';
  }

  /**
   * Parses the nested array reply returned by `XREAD` to extract the `p` field payload.
   */
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

  /**
   * Parses the latest entry id returned by `XREVRANGE stream + - COUNT 1`.
   */
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
