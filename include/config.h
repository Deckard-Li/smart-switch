#pragma once

// ── Config portal (AP mode) ───────────────────────────────────────────────────
// On first boot, or when saved WiFi credentials are missing/wrong, the device
// starts as an access point.  Connect to it and open http://192.168.4.1 to
// configure WiFi and MQTT server.  Settings are saved to EEPROM.
#define AP_SSID                "SmartSwitch-Setup"
#define AP_PASSWORD            ""          // leave empty for an open AP
#define WIFI_CONNECT_TIMEOUT_MS 15000UL   // how long to wait for WiFi before entering portal

// ── MQTT broker ───────────────────────────────────────────────────────────────
// WiFi SSID, password, and MQTT server address are configured via the web portal
// and stored in EEPROM – they are no longer compile-time constants.
#define MQTT_PORT       1883
#define MQTT_USER       ""              // leave empty if auth is not required
#define MQTT_PASSWORD   ""
#define MQTT_CLIENT_ID  "smart-switch"

// ── MQTT topics (command = subscribe, state = publish retained) ───────────────
//  Relay control
//    Publish "ON" or "OFF" to TOPIC_RELAY_CMD to turn the relay on/off.
#define TOPIC_RELAY_CMD   "home/smart-switch/relay/command"
#define TOPIC_RELAY_STATE "home/smart-switch/relay/state"

//  MAC-scan enable/disable
//    Publish "ON" or "OFF" to TOPIC_SCAN_CMD.
#define TOPIC_SCAN_CMD    "home/smart-switch/scan/command"
#define TOPIC_SCAN_STATE  "home/smart-switch/scan/state"

//  Target IP address
//    Publish an IP in "192.168.x.x" format to TOPIC_IP_CMD.
#define TOPIC_IP_CMD      "home/smart-switch/ip/command"
#define TOPIC_IP_STATE    "home/smart-switch/ip/state"

// ── Hardware ──────────────────────────────────────────────────────────────────
// ESP-01/01S has GPIO0 and GPIO2 available.
// Most relay shields for ESP-01 use GPIO0.
#define RELAY_PIN           0     // GPIO pin connected to the relay module
#define RELAY_ACTIVE_HIGH   false // true  → HIGH level turns relay ON
                                  // false → LOW  level turns relay ON (common for opto-isolated boards)


// ── IP presence scan (Ping-based) ────────────────────────────────────────────
// The device stays connected to WiFi at all times.  On each cycle it pings
// the target IP address.  No WiFi/MQTT reconnections are needed between cycles.
#define SCAN_INTERVAL_MS    30000UL  // how often to start a new ping cycle (ms)
