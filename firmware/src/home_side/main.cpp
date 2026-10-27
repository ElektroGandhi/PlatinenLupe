// Home side (ESP8266 NodeMCU or a second ESP32): serves the loupe to browsers. Three modes:
//
//   hotspot  (default) Opens its own WiFi "Platinenlupe-XXXX" (WPA2). Join it with any phone or laptop and the
//            loupe opens by itself (captive portal) at http://192.168.4.1. No router, no internet, no app.
//   home     Joins your WiFi and serves the same page at http://platinenlupe.local (or its IP).
//   server   Like home, and additionally behaves like the loupe itself on UDP 61500/61501 for a self-hosted
//            server (server/ in this repo) that adds component identification. It finds the board through a
//            broadcast on UDP 61499. The built-in page keeps working at the same time.
//
// If home or server mode cannot reach the configured WiFi within 30 s, the hotspot opens as well, so the
// settings are always reachable. Hold FLASH/BOOT for 5 s right after power-on to reset to factory settings.
//
// UART to the loupe side:
//   ESP8266: Serial.swap() moves UART0 to D7 = GPIO13 (RX) and D8 = GPIO15 (TX). Logs go to Serial1 (D4).
//   ESP32:   Serial2 on GPIO16 (RX) / GPIO17 (TX). Logs go to Serial (USB).
// LED: blinking = no WiFi client and no network, on = somebody is watching, off = ready.
#include <Arduino.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <WiFiUdp.h>

#if defined(ESP8266)
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
using WebServerT = ESP8266WebServer;
#define LINK Serial
#define LOG Serial1
#define PIN_LED 16           // D0, lights up on LOW
#define LED_ON LOW
#define AUTH_OPEN ENC_TYPE_NONE
#else
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
using WebServerT = WebServer;
#define LINK Serial2
#define LOG Serial
#ifndef PIN_LED
#define PIN_LED 2
#endif
#define LED_ON HIGH
#define AUTH_OPEN WIFI_AUTH_OPEN
#endif

#include "frame.h"
#include "page.h"   // generated from web/index.html by scripts/embed_page.py (gzip)

#ifndef LINK_BAUD
#define LINK_BAUD 2000000
#endif
#define PIN_RESET 0  // FLASH / BOOT button

static const char* FW_VERSION = "1.0.0";
static const char* DEFAULT_AP_PASSWORD = "platinenlupe";
static const char* HOSTNAME = "platinenlupe";
static const uint16_t PORT_VIDEO = 61501, PORT_CONTROL = 61500, PORT_BEACON = 61499;
static const uint8_t VIDEO_START[] = {0x20, 0x01}, VIDEO_STOP[] = {0x20, 0x02}, HEARTBEAT[] = {0x10, 0x01};

// ---- Settings (EEPROM) ---------------------------------------------------------------------
enum Mode : uint8_t { HOTSPOT = 0, HOME = 1, SERVER = 2 };
static const char* MODE_NAMES[] = {"hotspot", "home", "server"};

struct Settings {
  uint32_t magic;
  uint8_t mode;
  char apPassword[64];
  char wifiSsid[33];
  char wifiPassword[64];
  uint16_t crc;
};
static const uint32_t SETTINGS_MAGIC = 0x504C5531;   // "PLU1"
static Settings cfg;

static void settingsDefaults() {
  memset(&cfg, 0, sizeof cfg);
  cfg.magic = SETTINGS_MAGIC;
  cfg.mode = HOTSPOT;
  strncpy(cfg.apPassword, DEFAULT_AP_PASSWORD, sizeof cfg.apPassword - 1);
}

static uint16_t settingsCrc() { return frame::crc16((const uint8_t*)&cfg, offsetof(Settings, crc)); }

static void settingsLoad() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, cfg);
  if (cfg.magic != SETTINGS_MAGIC || cfg.crc != settingsCrc() || cfg.mode > SERVER) settingsDefaults();
  cfg.apPassword[63] = cfg.wifiSsid[32] = cfg.wifiPassword[63] = 0;
}

static void settingsSave() {
  cfg.crc = settingsCrc();
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---- Link to the loupe side ----------------------------------------------------------------
static WiFiUDP udpVideo, udpControl, udpBeacon;
static WebServerT web(80);
static DNSServer dns;
static frame::Reader reader;
static uint8_t linkBuf[frame::MAX_PAYLOAD + frame::OVERHEAD];

static String apSsid;
static bool apOn = false, staOn = false, fallback = false;
static uint32_t linkFull = 0, udpErrors = 0;

static void toLoupe(uint8_t type, const uint8_t* d, size_t n) {
  if (n > frame::MAX_PAYLOAD) return;
  size_t len = frame::build(type, d, n, linkBuf);
  if (LINK.availableForWrite() >= (int)len) LINK.write(linkBuf, len);
  else linkFull++;
}

// ---- Server mode: relay UDP --------------------------------------------------------------------
static IPAddress peerVideo, peerControl;
static uint16_t peerVideoPort = 0, peerControlPort = 0;
static uint32_t lastServerRequest = 0;

static bool serverWatching() { return cfg.mode == SERVER && millis() - lastServerRequest < 5000; }

static void relayIn(WiFiUDP& udp, uint8_t type, IPAddress& peer, uint16_t& port) {
  int n;
  while ((n = udp.parsePacket()) > 0) {
    peer = udp.remoteIP(); port = udp.remotePort(); lastServerRequest = millis();
    uint8_t d[frame::MAX_PAYLOAD];
    if (n > (int)sizeof d) { udp.flush(); continue; }
    udp.read(d, n);
    toLoupe(type, d, n);
  }
}

static void relayOut(WiFiUDP& udp, IPAddress peer, uint16_t port) {
  if (!port || !serverWatching()) return;   // nobody listening: save airtime
  if (!udp.beginPacket(peer, port)) { udpErrors++; return; }
  udp.write(reader.data, reader.len);
  if (!udp.endPacket()) udpErrors++;
}

// ---- Built-in page: one frame buffer -------------------------------------------------------
// The next frame is only collected once the previous one was fetched. The browser's fetch rate thus
// paces the frame rate, and no second buffer is needed (the ESP8266 has too little RAM for two).
static const size_t FRAME_MAX = 24 * 1024;           // measured: 6–21 KB per frame
static uint8_t* img = nullptr;                       // allocated after WiFi is up (see setup)
static size_t imgCapacity = 0, imgLen = 0;
enum ImgState { EMPTY, FILLING, READY };
static ImgState imgState = EMPTY;
static int imgNo = -1, imgTotal = 0, imgNext = 0;
static float imgAngle = 0;
static uint32_t lastViewer = 0, lastFrame = 0, framesServed = 0, tooBig = 0;
static uint8_t light = 70;
static String telemetry = "";                        // last JSON reply of the loupe
static String sideA = "null";                        // last status of the loupe side
static uint32_t lastSideA = 0;

static bool viewerActive() { return millis() - lastViewer < 5000; }

// The loupe side only forwards complete frames, chunk by chunk in order (24-byte header per chunk).
static void frameChunk(const uint8_t* d, size_t n) {
  if (n < 24 || !viewerActive() || imgState == READY) return;
  int no = d[0], seq = d[1], total = d[2];
  if (seq == 0) {
    imgState = FILLING; imgLen = 0; imgNo = no; imgTotal = total; imgNext = 0;
    memcpy(&imgAngle, d + 3, 4);
  }
  if (imgState != FILLING || no != imgNo || seq != imgNext) { imgState = EMPTY; return; }
  size_t part = n - 24;
  if (!img || imgLen + part > imgCapacity) { imgState = EMPTY; tooBig++; return; }
  memcpy(img + imgLen, d + 24, part);
  imgLen += part; imgNext++;
  if (imgNext == imgTotal) { imgState = READY; lastFrame = millis(); }
}

static void readLink(uint32_t waitMs = 0) {
  uint32_t t = millis();
  do {
    int n = 0;
    while (LINK.available() && n++ < 4096) {
      if (!reader.feed(LINK.read())) continue;
      if (reader.type == frame::VIDEO) {
        relayOut(udpVideo, peerVideo, peerVideoPort);
        frameChunk(reader.data, reader.len);
      } else if (reader.type == frame::CONTROL) {
        relayOut(udpControl, peerControl, peerControlPort);
        int a = -1;
        for (int i = 0; i < reader.len; i++) if (reader.data[i] == '{') { a = i; break; }
        if (a >= 0) { telemetry = ""; telemetry.concat((const char*)reader.data + a, reader.len - a); }
      } else if (reader.type == frame::STATUS) {
        sideA = "";
        sideA.concat((const char*)reader.data, reader.len);   // payload is not NUL-terminated
        lastSideA = millis();
      }
    }
    if (waitMs && imgState != READY) yield();
  } while (waitMs && imgState != READY && millis() - t < waitMs);
}

// ---- Web API -------------------------------------------------------------------------------
static String jsonField(const String& json, const char* name) {   // tiny reader for flat JSON values
  int i = json.indexOf(String("\"") + name + "\"");
  if (i < 0) return "";
  i = json.indexOf(':', i) + 1;
  while (i < (int)json.length() && json[i] == ' ') i++;
  if (json[i] == '"') { int e = json.indexOf('"', i + 1); return json.substring(i, e + 1); }
  int e = i;
  while (e < (int)json.length() && json[e] != ',' && json[e] != '}') e++;
  return json.substring(i, e);
}

static String jsonString(const String& s) {
  String o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if ((uint8_t)c >= 0x20) o += c;
  }
  return o + "\"";
}

static void sendPage() {
  lastViewer = millis();
  web.sendHeader("Content-Encoding", "gzip");
  web.sendHeader("Cache-Control", "no-cache");
  web.send_P(200, "text/html; charset=utf-8", (const char*)PAGE_GZ, PAGE_GZ_LEN);
}

// stage: live | wait | error; code tells the page which sentence to show (the page is bilingual).
static void sendState() {
  lastViewer = millis();
  bool aFresh = millis() - lastSideA < 5000;
  String s = aFresh ? jsonField(sideA, "state") : "";
  const char *stage = "wait", *code;
  if (millis() - lastFrame < 3000) { stage = "live"; code = "live"; }
  else if (!aFresh) { stage = "error"; code = "no_link"; }
  else if (s == "\"search\"") code = "searching";
  else if (s == "\"wake\"") code = "waking";
  else if (s == "\"join\"") code = "joining";
  else code = "first_frame";
  String t = telemetry, w;
  String j = "{\"device\":true,\"firmware\":\"";
  j += FW_VERSION; j += "\",\"mode\":\""; j += MODE_NAMES[cfg.mode];
  j += "\",\"fallback\":"; j += fallback ? "true" : "false";
  j += ",\"stage\":\""; j += stage; j += "\",\"code\":\""; j += code; j += "\",\"light\":"; j += light;
  if ((w = jsonField(t, "battery_percentage")).length()) {
    j += ",\"battery\":" + w;
    w = jsonField(t, "battery_charging");
    j += ",\"charging\":" + (w.length() ? w : String("false"));
  }
  if ((w = jsonField(t, "device_chip")).length()) j += ",\"chip\":" + w;
  if ((w = jsonField(t, "device_version")).length()) j += ",\"loupe_firmware\":" + w;
  if (jsonField(t, "width").length()) j += ",\"resolution\":\"" + jsonField(t, "width") + "×" + jsonField(t, "height") + "\"";
  if ((w = jsonField(t, "fps")).length()) j += ",\"camera_fps\":" + w;
  if ((w = jsonField(t, "debug_rssi")).length()) j += ",\"loupe_rssi\":" + w;
  j += ",\"ip\":\"" + (staOn ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"";
  j += ",\"server_watching\":"; j += serverWatching() ? "true" : "false";
  j += ",\"link\":{\"a\":" + (aFresh ? sideA : String("null"));
  j += ",\"b\":{\"rssi\":" + String(staOn ? WiFi.RSSI() : 0) + ",\"crc\":" + reader.crc_errors +
       ",\"heap\":" + ESP.getFreeHeap() + ",\"served\":" + framesServed + ",\"too_big\":" + tooBig +
       ",\"buffer\":" + imgCapacity + ",\"link_full\":" + linkFull + "}}}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

static void sendFrame() {
  lastViewer = millis();
  readLink(800);                                    // wait for the next complete frame
  if (imgState != READY) { web.send(503, "text/plain", "no frame"); return; }
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("X-Angle", String(imgAngle, 1));
  web.sendHeader("Access-Control-Expose-Headers", "X-Angle");
  web.setContentLength(imgLen);
  web.send(200, "image/jpeg", "");
  web.client().write(img, imgLen);
  framesServed++;
  imgState = EMPTY;                                 // only now collect the next frame
}

static void setLight() {
  lastViewer = millis();
  light = constrain(web.arg("v").toInt(), 0, 100);
  uint8_t cmd[] = {0x10, 0x03, light, 0x00};
  toLoupe(frame::CONTROL, cmd, sizeof cmd);
  web.send(200, "application/json", "{\"ok\":true}");
}

// Passwords never leave the device; the page only learns whether one is set.
static void sendSettings() {
  String j = "{\"mode\":\"";
  j += MODE_NAMES[cfg.mode];
  j += "\",\"ap_ssid\":" + jsonString(apSsid);
  j += ",\"ap_default_password\":"; j += strcmp(cfg.apPassword, DEFAULT_AP_PASSWORD) == 0 ? "true" : "false";
  j += ",\"wifi_ssid\":" + jsonString(cfg.wifiSsid);
  j += ",\"wifi_password_set\":"; j += cfg.wifiPassword[0] ? "true" : "false";
  j += ",\"hostname\":\""; j += HOSTNAME; j += ".local\"}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

static void error(const char* code) { web.send(400, "application/json", String("{\"ok\":false,\"error\":\"") + code + "\"}"); }

static void saveSettings() {
  String mode = web.arg("mode"), ssid = web.arg("wifi_ssid"), pass = web.arg("wifi_password"), ap = web.arg("ap_password");
  int m = mode == "home" ? HOME : mode == "server" ? SERVER : mode == "hotspot" ? HOTSPOT : -1;
  if (m < 0) return error("mode");
  if (m != HOTSPOT && ssid.isEmpty() && !cfg.wifiSsid[0]) return error("wifi_ssid");
  if (ssid.length() > 32 || pass.length() > 63) return error("wifi_length");
  if (ap.length() && (ap.length() < 8 || ap.length() > 63)) return error("ap_password");
  cfg.mode = m;
  if (ssid.length()) {
    if (ssid != cfg.wifiSsid) memset(cfg.wifiPassword, 0, sizeof cfg.wifiPassword);   // new network, new password
    strncpy(cfg.wifiSsid, ssid.c_str(), sizeof cfg.wifiSsid - 1);
  }
  if (pass.length()) strncpy(cfg.wifiPassword, pass.c_str(), sizeof cfg.wifiPassword - 1);
  if (ap.length()) { memset(cfg.apPassword, 0, sizeof cfg.apPassword); strncpy(cfg.apPassword, ap.c_str(), sizeof cfg.apPassword - 1); }
  settingsSave();
  web.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
  delay(300);
  ESP.restart();
}

static void sendScan() {
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n && i < 20; i++) {
    if (WiFi.SSID(i).isEmpty()) continue;
    if (j.length() > 1) j += ',';
    j += "{\"ssid\":" + jsonString(WiFi.SSID(i)) + ",\"rssi\":" + WiFi.RSSI(i) +
         ",\"open\":" + (WiFi.encryptionType(i) == AUTH_OPEN ? "true" : "false") + "}";
  }
  WiFi.scanDelete();
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j + "]");
}

// Phones probe a known URL after joining a WiFi; answering with a redirect makes them open the loupe.
static void notFound() {
  if (apOn && web.hostHeader() != WiFi.softAPIP().toString()) {
    web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    web.send(302, "text/plain", "");
    return;
  }
  web.send(404, "text/plain", "not found");
}

// ---- Keep the loupe awake while somebody watches the built-in page --------------------------
static void localTick() {
  static uint32_t last = 0, n = 0;
  static bool wasActive = false;
  if (millis() - last < 1000) return;
  last = millis();
  if (viewerActive()) {
    // every second: start + heartbeat (without a sign of life the loupe switches off after 3 s)
    toLoupe(frame::VIDEO, VIDEO_START, sizeof VIDEO_START);
    toLoupe(frame::CONTROL, HEARTBEAT, sizeof HEARTBEAT);
    if (n++ % 5 == 1) { uint8_t b[] = {0x10, 0x03, light, 0x00}; toLoupe(frame::CONTROL, b, sizeof b); }
    wasActive = true;
  } else if (wasActive && millis() - lastViewer > 30000) {
    if (!serverWatching())                          // only stop if no server is watching either
      for (int i = 0; i < 3; i++) toLoupe(frame::VIDEO, VIDEO_STOP, sizeof VIDEO_STOP);
    wasActive = false; imgState = EMPTY;
  }
}

static void beacon() {
  bool aFresh = millis() - lastSideA < 5000;
  char b[200];
  snprintf(b, sizeof b,
           "\"b\":{\"rssi\":%d,\"crc\":%u,\"link_full\":%u,\"udp_errors\":%u,\"uptime\":%lu,\"web\":true}",
           WiFi.RSSI(), reader.crc_errors, linkFull, udpErrors, millis() / 1000);
  String s = String("{\"device\":\"platinenlupe\",\"v\":2,\"firmware\":\"") + FW_VERSION + "\",\"a\":" +
             (aFresh ? sideA : "null") + "," + b + "}";
  IPAddress bc = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  for (int i = 0; i < 4; i++) bc[i] = bc[i] | ~mask[i];
  udpBeacon.beginPacket(bc, PORT_BEACON);
  udpBeacon.write((const uint8_t*)s.c_str(), s.length());
  udpBeacon.endPacket();
}

// ---- WiFi ------------------------------------------------------------------------------------
static void startHotspot() {
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid.c_str(), cfg.apPassword);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  LOG.printf("Hotspot %s at %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

static void startWifi() {
#if defined(ESP8266)
  apSsid = "Platinenlupe-" + String(ESP.getChipId() & 0xFFFF, HEX);
  WiFi.hostname(HOSTNAME);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#else
  apSsid = "Platinenlupe-" + String((uint32_t)(ESP.getEfuseMac() >> 32) & 0xFFFF, HEX);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);
#endif
  apSsid.toUpperCase();
  apSsid.replace("PLATINENLUPE", "Platinenlupe");
  WiFi.persistent(false);
  if (cfg.mode == HOTSPOT) {
    WiFi.mode(WIFI_AP);
    startHotspot();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
  LOG.printf("Joining %s ", cfg.wifiSsid);
  for (uint32_t t = millis(); millis() - t < 30000 && WiFi.status() != WL_CONNECTED;) {
    digitalWrite(PIN_LED, (millis() / 150) % 2);
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    staOn = true;
    LOG.printf("ok, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    LOG.println("failed — opening the hotspot as well");
    fallback = true;
    WiFi.mode(WIFI_AP_STA);
    startHotspot();
  }
}

static void factoryResetIfHeld() {
  uint32_t t = millis();
  while (digitalRead(PIN_RESET) == LOW && millis() - t < 5000) {
    digitalWrite(PIN_LED, (millis() / 100) % 2);
    delay(10);
  }
  if (millis() - t >= 5000) {
    LOG.println("Reset button held: factory settings");
    settingsDefaults();
    settingsSave();
  }
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_RESET, INPUT_PULLUP);
  LOG.begin(115200);
  settingsLoad();
  factoryResetIfHeld();
  LOG.printf("\nPlatinenlupe home side %s, mode %s\n", FW_VERSION, MODE_NAMES[cfg.mode]);
  startWifi();

#if defined(ESP8266)
  LINK.setRxBufferSize(8192);   // WiFi sends sometimes stall; the buffer absorbs that
  LINK.begin(LINK_BAUD);
  LINK.swap();                  // UART0 away from USB, onto D7/D8
#else
  LINK.setRxBufferSize(16384);
  LINK.begin(LINK_BAUD, SERIAL_8N1, 16, 17);
#endif
  // Allocate the frame buffer last: the WiFi stack needs its share of the ESP8266's RAM first.
  for (imgCapacity = FRAME_MAX; imgCapacity >= 8 * 1024 && !(img = (uint8_t*)malloc(imgCapacity)); imgCapacity -= 4 * 1024) {}
  if (!img) imgCapacity = 0;
  LOG.printf("Frame buffer %u B, free heap %u B\n", imgCapacity, ESP.getFreeHeap());

  if (cfg.mode == SERVER) {
    udpVideo.begin(PORT_VIDEO);
    udpControl.begin(PORT_CONTROL);
    udpBeacon.begin(PORT_BEACON + 1);   // send only; own port so no echo arrives
  }
  web.on("/", sendPage);
  web.on("/api/state", sendState);
  web.on("/api/frame.jpg", sendFrame);
  web.on("/api/light", HTTP_POST, setLight);
  web.on("/api/settings", HTTP_GET, sendSettings);
  web.on("/api/settings", HTTP_POST, saveSettings);
  web.on("/api/scan", sendScan);
  web.onNotFound(notFound);
  web.begin();
  if (staOn) {
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  }
}

void loop() {
  if (apOn) dns.processNextRequest();
  if (cfg.mode == SERVER && staOn) {
    relayIn(udpVideo, frame::VIDEO, peerVideo, peerVideoPort);
    relayIn(udpControl, frame::CONTROL, peerControl, peerControlPort);
  }
  readLink();
  web.handleClient();
#if defined(ESP8266)
  if (staOn) MDNS.update();
#endif
  localTick();
  static uint32_t lastBeacon = 0;
  if (cfg.mode == SERVER && staOn && millis() - lastBeacon >= 2000) {
    lastBeacon = millis();
    beacon();
  }
  staOn = cfg.mode != HOTSPOT && WiFi.status() == WL_CONNECTED;
  bool lonely = !staOn && !(apOn && WiFi.softAPgetStationNum() > 0);
  bool on = lonely ? (millis() / 300) % 2 : (serverWatching() || viewerActive());
  digitalWrite(PIN_LED, on ? LED_ON : !LED_ON);
  yield();
}
