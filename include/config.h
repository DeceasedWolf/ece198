#pragma once

/**
 * Copy this file to include/config.h and edit the values to match your lab or demo
 * network before flashing any of the PlatformIO projects.
 */

// Wi-Fi credentials for every ESP8266 in the system.
#define WIFI_SSID "DECEASED"
#define WIFI_PASS "12345678"
#define WIFI_HOSTNAME "hospital-light-node"

// Redis broker configuration.
#define REDIS_HOST "192.168.137.61"
#define REDIS_PORT 6379
#define REDIS_PASSWORD ""

// Optional overrides for early bring-up. Leave empty to rely on provisioning.
#define ROOM_ID_OVERRIDE "100"
#define PROVISIONING_BASE_ID 100

// How often (in milliseconds) the receiver announces its room id over UART.
#define ROOM_ANNOUNCE_INTERVAL_MS 3000

// Heartbeat interval for the receiver (must be < kHeartbeatTtlSec from contracts.hpp).
#define RECEIVER_HEARTBEAT_MS 3000

// Receiver LED pin/polarity (set ACTIVE_LOW when the LED turns on if the pin is driven low).
#define RECEIVER_LED_PIN D5
#define RECEIVER_LED_ACTIVE_LOW 0

// Optional RGB channel wiring. Set any unused channel to -1. When at least one
// channel is enabled the mix percentages below drive the overall color.
#define RECEIVER_LED_RED_PIN D1
#define RECEIVER_LED_GREEN_PIN D2
#define RECEIVER_LED_BLUE_PIN D5

// Channel mix (0-100) targeting RGB(255,254,208) on the common-cathode LED.
#define RECEIVER_LED_RED_PERCENT 100
#define RECEIVER_LED_GREEN_PERCENT 99
#define RECEIVER_LED_BLUE_PERCENT 55

// Optional status LED (set to LED_BUILTIN to mirror the applied brightness, or -1 to disable).
#define RECEIVER_STATUS_LED_PIN LED_BUILTIN
#define RECEIVER_STATUS_LED_ACTIVE_LOW 1

// Quiet-hours + sound monitoring (receiver).
#define RECEIVER_CFG_REFRESH_MS 30000
#define RECEIVER_SOUND_SENSOR_PIN A0
#define RECEIVER_SOUND_SAMPLE_INTERVAL_MS 200
#define RECEIVER_SOUND_AVERAGE_SAMPLES 8
#define RECEIVER_SOUND_SENSOR_MIN_DB 30.0f
#define RECEIVER_SOUND_SENSOR_MAX_DB 110.0f
#define RECEIVER_SOUND_WARNING_THRESHOLD_DB 70.0f
#define RECEIVER_SOUND_WARNING_COOLDOWN_MS 15000

// ESP sender console + scheduling defaults.
#define SENDER_CONSOLE_BAUD 115200
#define SCHEDULE_REFRESH_MS 30000
#define SCHEDULE_PUBLISH_MIN_INTERVAL_MS 1000
#define SCHEDULE_DEFAULT_WAKE_HOUR 7
#define SCHEDULE_DEFAULT_WAKE_MINUTE 0
#define SCHEDULE_DEFAULT_WAKE_DURATION_MIN 20
#define SCHEDULE_DEFAULT_WAKE_BRIGHTNESS 100
#define SCHEDULE_DEFAULT_NIGHT_HOUR 22
#define SCHEDULE_DEFAULT_NIGHT_MINUTE 0
#define SCHEDULE_DEFAULT_NIGHT_BRIGHTNESS 0
#define SCHEDULE_DEFAULT_BASELINE_BRIGHTNESS 0
#define QUIET_HOURS_DIM_MINUTES 90
#define WAKE_BRIGHTEN_MINUTES 30

// Warning banner duration for the sender display (milliseconds).
#define SOUND_WARNING_DISPLAY_MS 15000

// Manual override (control panel hardware).
#define OVERRIDE_POT_PIN A0
#define OVERRIDE_BUTTON_PIN D5
#define OVERRIDE_BUTTON_PIN_MODE INPUT_PULLUP
#define OVERRIDE_BUTTON_ACTIVE_LEVEL LOW
#define OVERRIDE_BUTTON_DEBOUNCE_MS 50
#define OVERRIDE_ANALOG_MIN 0
#define OVERRIDE_ANALOG_MAX 1023
#define OVERRIDE_ANALOG_MIN_DELTA 2

// Small status OLED (defaults assume I2C wiring: SCL->D1, SDA->D2, power from 3.3V).
#define SENDER_DISPLAY_ENABLED 1
#define SENDER_DISPLAY_SCL_PIN D1
#define SENDER_DISPLAY_SDA_PIN D2
#define SENDER_DISPLAY_I2C_ADDRESS 0x3C
#define SENDER_DISPLAY_REFRESH_INTERVAL_MS 1000

// Timekeeping (seconds relative to UTC). Adjust for deployment timezone/DST.
#define TZ_OFFSET_SECONDS (-5 * 3600)
#define DST_OFFSET_SECONDS 0
#define NTP_SERVER_PRIMARY "pool.ntp.org"
#define NTP_SERVER_SECONDARY "time.nist.gov"
#define NTP_SERVER_TERTIARY "time.cloudflare.com"
