#pragma once

/**
 * Copy this file to include/config.h and edit the values to match your lab or demo
 * network before flashing any of the PlatformIO projects.
 */

// Wi-Fi credentials for every ESP8266 in the system.
#define WIFI_SSID "ChangeMe"
#define WIFI_PASS "ChangeMeToo"
#define WIFI_HOSTNAME "hospital-light-node"

// Redis broker configuration.
#define REDIS_HOST "192.168.1.50"
#define REDIS_PORT 6379
#define REDIS_PASSWORD ""

// Optional overrides for early bring-up. Leave empty to rely on provisioning.
#define ROOM_ID_OVERRIDE ""
#define PROVISIONING_BASE_ID 100

// How often (in milliseconds) the receiver announces its room id over UART.
#define ROOM_ANNOUNCE_INTERVAL_MS 3000

// Heartbeat interval for the receiver (must be < kHeartbeatTtlSec from contracts.hpp).
#define RECEIVER_HEARTBEAT_MS 3000

// UART housekeeping for the sender panel.
#define PANEL_UART_BAUD 115200
#define ROOM_LINK_BAUD 115200
#define PANEL_FRAME_TIMEOUT_MS 50
#define PANEL_FRAME_MAX_BYTES 128

