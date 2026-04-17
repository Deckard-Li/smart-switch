#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include "config.h"

#include <lwip/etharp.h>         // etharp_request(), etharp_get_entry()
#include <lwip/netif.h>          // netif_default

// ── EEPROM layout ─────────────────────────────────────────────────────────────
struct StoredConfig {
  char     wifiSsid[32];
  char     wifiPassword[64];
  char     mqttServer[64];
  uint16_t mqttPort;
  uint8_t  mac[6];
  bool     scanEnabled;
  bool     macSet;
  bool     masterOn;
  uint8_t  checksum;   // XOR checksum of all preceding bytes
};

// ── Runtime credentials (loaded from EEPROM) ──────────────────────────────────
char     netSsid[32]       = {0};
char     netPassword[64]   = {0};
char     netMqttServer[64] = {0};
uint16_t netMqttPort       = MQTT_PORT;   // default; overwritten from EEPROM

// ── Config portal ─────────────────────────────────────────────────────────────
ESP8266WebServer webServer(80);
DNSServer        dnsServer;
bool             portalActive = false;

// ── Runtime state ─────────────────────────────────────────────────────────────
uint8_t  targetMac[6]  = {0};
bool     targetMacSet  = false;
bool     scanEnabled   = false;
bool     masterOn      = false;  // relay/command master switch
bool     relayState    = false;
bool          arpPending   = false;  // true while waiting for ARP replies
unsigned long lastScanTime = 0;
unsigned long arpSentTime  = 0;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ── EEPROM helpers ────────────────────────────────────────────────────────────
static uint8_t calcChecksum(const StoredConfig& cfg) {
  uint8_t cs = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&cfg);
  for (size_t i = 0; i < sizeof(StoredConfig) - 1; i++) cs ^= p[i];
  return cs;
}

void loadConfig() {
  EEPROM.begin(sizeof(StoredConfig));
  StoredConfig cfg;
  EEPROM.get(0, cfg);

  // A completely erased flash is filled with 0xFF. By coincidence, the XOR checksum
  // of 171 0xFF bytes is also 0xFF, so we must explicitly reject a wiped EEPROM.
  if (calcChecksum(cfg) == cfg.checksum && cfg.wifiSsid[0] != '\xFF') {
    strncpy(netSsid,       cfg.wifiSsid,     sizeof(netSsid)       - 1);
    strncpy(netPassword,   cfg.wifiPassword, sizeof(netPassword)   - 1);
    strncpy(netMqttServer, cfg.mqttServer,   sizeof(netMqttServer) - 1);
    netMqttPort = cfg.mqttPort ? cfg.mqttPort : MQTT_PORT;
    memcpy(targetMac, cfg.mac, 6);
    scanEnabled  = cfg.scanEnabled;
    targetMacSet = cfg.macSet;
    masterOn     = cfg.masterOn;
    Serial.println("[EEPROM] Config restored");
    Serial.printf("[EEPROM] WiFi SSID:   %s\n", netSsid);
    Serial.printf("[EEPROM] MQTT server: %s:%u\n", netMqttServer, netMqttPort);
  }
}

void saveConfig() {
  StoredConfig cfg = {};
  strncpy(cfg.wifiSsid,     netSsid,       sizeof(cfg.wifiSsid)     - 1);
  strncpy(cfg.wifiPassword, netPassword,   sizeof(cfg.wifiPassword) - 1);
  strncpy(cfg.mqttServer,   netMqttServer, sizeof(cfg.mqttServer)   - 1);
  cfg.mqttPort = netMqttPort;
  memcpy(cfg.mac, targetMac, 6);
  cfg.scanEnabled = scanEnabled;
  cfg.macSet      = targetMacSet;
  cfg.masterOn    = masterOn;
  cfg.checksum    = calcChecksum(cfg);
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ── Relay helpers ─────────────────────────────────────────────────────────────
void setRelayHardware(bool on) {
  relayState = on;
  bool level = RELAY_ACTIVE_HIGH ? on : !on;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

void publishRelayState() {
  mqttClient.publish(TOPIC_RELAY_STATE, relayState ? "ON" : "OFF", true);
}

void setRelay(bool on) {
  setRelayHardware(on);
  publishRelayState();
}

// ── MAC helpers ───────────────────────────────────────────────────────────────
// Accepts "AA:BB:CC:DD:EE:FF" (upper or lower case).
bool parseMac(const char* str, uint8_t* mac) {
  if (strlen(str) != 17) return false;
  for (int i = 0; i < 6; i++) {
    if (i < 5 && str[i * 3 + 2] != ':') return false;
    char b[3] = {str[i * 3], str[i * 3 + 1], '\0'};
    mac[i] = (uint8_t)strtol(b, nullptr, 16);
  }
  return true;
}

void publishMacState() {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
    targetMac[0], targetMac[1], targetMac[2],
    targetMac[3], targetMac[4], targetMac[5]);
  mqttClient.publish(TOPIC_MAC_STATE, buf, true);
}

// ── ARP-based presence detection ─────────────────────────────────────────────
// Send one ARP request per host address in the local /24 subnet.
// lwIP processes the replies automatically while mqttClient.loop()/yield() run.
void sendArpRequests() {
  struct netif* iface = netif_default;
  if (!iface) return;
  IPAddress local = WiFi.localIP();
  Serial.printf("[Scan] Sending ARP requests on %d.%d.%d.0/24\n",
                local[0], local[1], local[2]);
  for (int h = 1; h < 255; h++) {
    ip4_addr_t target;
    IP4_ADDR(&target, local[0], local[1], local[2], (uint8_t)h);
    etharp_request(iface, &target);
    delay(1);   // let lwIP breathe between requests
  }
}

// Scan the lwIP ARP cache for the target MAC address.
bool checkArpCache() {
  for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
    ip4_addr_t*      ip  = nullptr;
    struct netif*    nif = nullptr;
    struct eth_addr* mac = nullptr;
    if (etharp_get_entry(i, &ip, &nif, &mac) && mac) {
      if (memcmp(mac->addr, targetMac, 6) == 0) return true;
    }
  }
  return false;
}

// ── Config portal ─────────────────────────────────────────────────────────────
// Starts the ESP as a WiFi access point and serves a configuration page at
// http://192.168.4.1.  All DNS queries are redirected there (captive portal).
void startConfigPortal() {
  portalActive = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);   // redirect all DNS to our IP

  // ── GET / – show configuration form ────────────────────────────────────────
  webServer.on("/", HTTP_GET, []() {
    // Stream in small PROGMEM chunks to avoid allocating a large heap String.
    webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    webServer.send(200, F("text/html; charset=utf-8"), "");

    webServer.sendContent_P(PSTR(
      "<!DOCTYPE html><html><head>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Smart Switch Setup</title><style>"
      "body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}"
      "h2{color:#333}label{display:block;margin-top:14px;font-size:.9em;color:#555}"
      "input{width:100%;padding:8px;margin-top:4px;border:1px solid #ccc;"
      "border-radius:4px;box-sizing:border-box}"
      "button{width:100%;padding:12px;margin-top:22px;background:#0066cc;"
      "color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer}"
      "button:hover{background:#0055aa}"
      "</style></head><body>"
      "<h2>Smart Switch Setup</h2>"
      "<form method='POST' action='/save'>"
      "<label>WiFi SSID</label>"
      "<input name='ssid' value='"));
    if (netSsid[0]) webServer.sendContent(netSsid);

    webServer.sendContent_P(PSTR(
      "'><label>WiFi Password</label>"
      "<input name='pass' type='password' placeholder='leave blank to keep current'>"
      "<label>MQTT Server</label>"
      "<input name='mqtt' placeholder='192.168.1.x' value='"));
    if (netMqttServer[0]) webServer.sendContent(netMqttServer);

    char portBuf[6];
    snprintf(portBuf, sizeof(portBuf), "%u", netMqttPort);
    webServer.sendContent_P(PSTR("'><label>MQTT Port</label>"
      "<input name='port' type='number' min='1' max='65535' value='"));
    webServer.sendContent(portBuf);

    webServer.sendContent_P(PSTR(
      "'><button type='submit'>Save &amp; Restart</button>"
      "</form></body></html>"));
    webServer.sendContent(""); // Send empty chunk to terminate the response
  });

  // ── POST /save – persist credentials and reboot ─────────────────────────────
  webServer.on("/save", HTTP_POST, []() {
    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");
    String mqtt = webServer.arg("mqtt");
    String port = webServer.arg("port");
    if (ssid.length() > 0) ssid.toCharArray(netSsid,       sizeof(netSsid));
    if (pass.length() > 0) pass.toCharArray(netPassword,   sizeof(netPassword));
    if (mqtt.length() > 0) mqtt.toCharArray(netMqttServer, sizeof(netMqttServer));
    if (port.length() > 0) netMqttPort = (uint16_t)constrain(port.toInt(), 1, 65535);
    saveConfig();
    webServer.send(200, F("text/html"),
      F("<!DOCTYPE html><html><body style='font-family:sans-serif;"
        "text-align:center;padding-top:60px'>"
        "<h2>Saved!</h2>"
        "<p>Restarting&hellip; reconnect to your WiFi network.</p>"
        "</body></html>"));
    delay(1500);
    ESP.restart();
  });

  // Captive-portal redirect for any other path
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", F("http://192.168.4.1/"));
    webServer.send(302, F("text/plain"), "");
  });

  webServer.begin();
  Serial.printf("[Portal] AP '%s' up at %s\n", AP_SSID, apIP.toString().c_str());
}

// ── WiFi helpers ──────────────────────────────────────────────────────────────
// Returns true if connected, false if timed out (timeoutMs=0 → wait forever).
bool connectWiFi(unsigned long timeoutMs = 0) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.mode(WIFI_STA);
  WiFi.begin(netSsid, netPassword);
  Serial.print("[WiFi] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (timeoutMs && (millis() - start >= timeoutMs)) {
      Serial.println(" timed out");
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Connected – IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void publishAllStates() {
  publishRelayState();
  mqttClient.publish(TOPIC_SCAN_STATE, scanEnabled ? "ON" : "OFF", true);
  if (targetMacSet) publishMacState();
}

// Forward declaration so connectMQTT can register the callback before it is defined.
void mqttCallback(char*, byte*, unsigned int);

void connectMQTT() {
  mqttClient.setServer(netMqttServer, netMqttPort);
  mqttClient.setCallback(mqttCallback);
  Serial.printf("[MQTT] Server: '%s'  Port: %u\n", netMqttServer, netMqttPort);
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Connecting...");
    bool ok = (strlen(MQTT_USER) > 0)
      ? mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqttClient.connect(MQTT_CLIENT_ID);
    if (ok) {
      Serial.println(" OK");
      mqttClient.subscribe(TOPIC_RELAY_CMD);
      mqttClient.subscribe(TOPIC_SCAN_CMD);
      mqttClient.subscribe(TOPIC_MAC_CMD);
      publishAllStates();
    } else {
      Serial.printf(" failed to connect to '%s:%u' (rc=%d), retrying in 3 s\n", netMqttServer, netMqttPort, mqttClient.state());
      delay(3000);
    }
  }
}

// ── MQTT message handler ──────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];
  length = min(length, (unsigned int)(sizeof(msg) - 1));
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (strcmp(topic, TOPIC_RELAY_CMD) == 0) {
    // relay/command is the master switch.
    // OFF → shut everything down immediately, scan suspended.
    // ON  → activate master; scan takes control if enabled, otherwise relay on.
    if (strcmp(msg, "OFF") == 0) {
      masterOn   = false;
      arpPending = false;
      saveConfig();
      setRelay(false);
    } else if (strcmp(msg, "ON") == 0) {
      masterOn = true;
      saveConfig();
      if (scanEnabled && targetMacSet) {
        lastScanTime = 0;    // trigger an immediate ARP scan cycle
      } else {
        setRelay(true);      // no scan configured – just turn on
      }
    }
  }
  else if (strcmp(topic, TOPIC_SCAN_CMD) == 0) {
    // scan/command only governs the scan feature, never touches masterOn.
    if      (strcmp(msg, "ON")  == 0) { scanEnabled = true;  lastScanTime = 0; }
    else if (strcmp(msg, "OFF") == 0) { scanEnabled = false; arpPending = false; }
    mqttClient.publish(TOPIC_SCAN_STATE, scanEnabled ? "ON" : "OFF", true);
    saveConfig();
  }
  else if (strcmp(topic, TOPIC_MAC_CMD) == 0) {
    // Payload must be "AA:BB:CC:DD:EE:FF" (colons, upper or lower case hex).
    if (parseMac(msg, targetMac)) {
      targetMacSet = true;
      publishMacState();
      saveConfig();
      Serial.printf("[MAC] Target set to %s\n", msg);
    } else {
      Serial.printf("[MAC] Invalid format: %s\n", msg);
    }
  }
}

// ── Scan lifecycle ────────────────────────────────────────────────────────────
void startScan() {
  sendArpRequests();
  arpSentTime = millis();
  arpPending  = true;
}

void finishScan() {
  arpPending   = false;
  lastScanTime = millis();
  if (!masterOn) return;        // master went off while scan was pending – do nothing
  bool found = checkArpCache();
  Serial.printf("[Scan] ARP cache check – target MAC %s\n", found ? "FOUND" : "not found");
  setRelay(found);
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  setRelayHardware(false);      // relay off at boot

  loadConfig();                 // restore credentials + masterOn + scanEnabled + MAC

  // Try to connect with stored credentials; fall back to config portal if:
  //   • no SSID has been saved yet, or
  //   • connection times out.
  bool wifiOk = (netSsid[0] != '\0') && connectWiFi(WIFI_CONNECT_TIMEOUT_MS);
  if (!wifiOk) {
    startConfigPortal();
    return;                     // loop() will handle portal traffic only
  }

  connectMQTT();

  // Re-apply master state after reconnecting.
  if (masterOn) {
    if (scanEnabled && targetMacSet) lastScanTime = 0;  // scan will fire immediately
    else                             setRelay(true);
  }
}

void loop() {
  // Portal mode: serve DNS + web requests until the user saves and the device reboots.
  if (portalActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected())        connectMQTT();
  mqttClient.loop();

  unsigned long now = millis();

  // Phase 1 – send ARP requests when master is on and interval has elapsed.
  if (masterOn && scanEnabled && targetMacSet && !arpPending &&
      (now - lastScanTime >= SCAN_INTERVAL_MS)) {
    startScan();
  }

  // Phase 2 – read the ARP cache after replies have had time to arrive.
  if (arpPending && (now - arpSentTime >= ARP_REPLY_TIMEOUT_MS)) {
    finishScan();
  }
}
