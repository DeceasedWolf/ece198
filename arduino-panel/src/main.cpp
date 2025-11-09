#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"

namespace {

constexpr uint8_t kPotPin = A0;
constexpr uint8_t kButtonPin = 2;
constexpr uint8_t kPotSamples = 4;
constexpr uint16_t kAdcMax = 4095;
constexpr uint8_t kBrightnessStep = 2;
constexpr unsigned long kMaxFrameIntervalMs = 1000;
constexpr unsigned long kButtonDebounceMs = 30;

struct PanelState {
  bool isOn = false;
  uint8_t brightness = 0;
};

PanelState lastSent;
bool lastButtonLevel = true;
unsigned long lastButtonChangeMs = 0;
unsigned long lastFrameMs = 0;

uint8_t sampleBrightness() {
  uint32_t accum = 0;
  for (uint8_t i = 0; i < kPotSamples; ++i) {
    accum += analogRead(kPotPin);
  }
  uint32_t raw = accum / kPotSamples;
  if (raw > kAdcMax) {
    raw = kAdcMax;
  }
  return static_cast<uint8_t>((raw * 100U) / kAdcMax);
}

bool readButtonStable(unsigned long now) {
  bool level = digitalRead(kButtonPin) == LOW;
  if (level != lastButtonLevel && (now - lastButtonChangeMs) > kButtonDebounceMs) {
    lastButtonLevel = level;
    lastButtonChangeMs = now;
    if (level) {
      return true;
    }
  }
  return false;
}

void sendState(const PanelState &state) {
  StaticJsonDocument<128> doc;
  doc["mode"] = state.isOn ? "on" : "off";
  doc["brightness"] = state.brightness;
  serializeJson(doc, Serial1);
  Serial1.print('\n');
  lastFrameMs = millis();
  Serial.print(F("Panel -> "));
  serializeJson(doc, Serial);
  Serial.println();
}

bool shouldSend(const PanelState &candidate, unsigned long now) {
  if (lastFrameMs == 0) {
    return true;
  }
  if (candidate.isOn != lastSent.isOn) {
    return true;
  }
  if (candidate.brightness + kBrightnessStep < lastSent.brightness ||
      candidate.brightness > lastSent.brightness + kBrightnessStep) {
    return true;
  }
  return (now - lastFrameMs) >= kMaxFrameIntervalMs;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial1.begin(PANEL_UART_BAUD);
  pinMode(kButtonPin, INPUT_PULLUP);
  analogReadResolution(12);
  Serial.println(F("panel ready"));
}

void loop() {
  unsigned long now = millis();
  PanelState current = lastSent;
  current.brightness = sampleBrightness();
  if (readButtonStable(now)) {
    current.isOn = !current.isOn;
  }
  if (!current.isOn) {
    current.brightness = 0;
  }
  if (shouldSend(current, now)) {
    sendState(current);
    lastSent = current;
  }
  delay(10);
}
