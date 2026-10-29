// Home side (ESP8266 NodeMCU or a second ESP32): serves the loupe to browsers. Two modes:
//
//   hotspot  (default) Opens its own WiFi "Platinenlupe-XXXX" (WPA2). Join it with any phone or laptop and the
//            loupe opens by itself (captive portal) at http://192.168.4.1. No router, no internet, no app.
//   home     Joins your WiFi and serves the same page at its address (computers also: http://platinenlupe.local).
//
// Part identification needs no server: the browser cuts out the focus ring and asks a vision model in your
// network directly (LM Studio, Ollama, OpenAI-compatible). The board only stores the model's address.
//
// If home mode cannot reach the configured WiFi within 30 s, the hotspot opens as well, so the settings are
// always reachable. Hold FLASH/BOOT for 5 s right after power-on to reset to factory settings.
//
// UART to the loupe side:
//   ESP8266: Serial.swap() moves UART0 to D7 = GPIO13 (RX) and D8 = GPIO15 (TX). Logs go to Serial1 (D4).
//   ESP32:   Serial2 on GPIO16 (RX) / GPIO17 (TX). Logs go to Serial (USB).
// LED: blinking = no WiFi client and no network, on = somebody is watching, off = ready.
#include <Arduino.h>
#include <DNSServer.h>
#include <EEPROM.h>

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
#define LINK_BAUD 1000000
#endif
#define PIN_RESET 0  // FLASH / BOOT button

static const char* FW_VERSION = "1.1.0";
static const char* DEFAULT_AP_PASSWORD = "platinenlupe";
static const char* HOSTNAME = "platinenlupe";
static const uint8_t VIDEO_START[] = {0x20, 0x01}, VIDEO_STOP[] = {0x20, 0x02}, HEARTBEAT[] = {0x10, 0x01};

// ---- Settings (EEPROM) ---------------------------------------------------------------------
enum Mode : uint8_t { HOTSPOT = 0, HOME = 1 };
static const char* MODE_NAMES[] = {"hotspot", "home"};

struct Settings {
  uint32_t magic;
  uint8_t mode;
  char apPassword[64];
  char wifiSsid[33];
  char wifiPassword[64];
  char modelUrl[96];         // e.g. http://192.168.1.20:1234 — empty: no part identification
  char modelName[64];        // empty: LM Studio picks the loaded vision model
  uint16_t crc;
};
static const uint32_t SETTINGS_MAGIC = 0x504C5532;   // "PLU2"
static Settings cfg;

// Firmware 1.0 stored this layout ("PLU1", with a third mode "server"). Read it once so that an update keeps the WiFi.
struct SettingsV1 {
  uint32_t magic;
  uint8_t mode;
  char apPassword[64];
  char wifiSsid[33];
  char wifiPassword[64];
  uint16_t crc;
};

static void settingsDefaults() {
  memset(&cfg, 0, sizeof cfg);
  cfg.magic = SETTINGS_MAGIC;
  cfg.mode = HOTSPOT;
  strncpy(cfg.apPassword, DEFAULT_AP_PASSWORD, sizeof cfg.apPassword - 1);
}

static uint16_t settingsCrc() { return frame::crc16((const uint8_t*)&cfg, offsetof(Settings, crc)); }

static void settingsSave() {
  cfg.crc = settingsCrc();
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

static void settingsLoad() {
  EEPROM.begin(sizeof(Settings));
  EEPROM.get(0, cfg);
  if (cfg.magic == SETTINGS_MAGIC && cfg.crc == settingsCrc() && cfg.mode <= HOME) {
    cfg.apPassword[63] = cfg.wifiSsid[32] = cfg.wifiPassword[63] = cfg.modelUrl[95] = cfg.modelName[63] = 0;
    return;
  }
  SettingsV1 old;
  EEPROM.get(0, old);
  bool oldOk = old.magic == 0x504C5531 &&
               old.crc == frame::crc16((const uint8_t*)&old, offsetof(SettingsV1, crc)) && old.mode <= 2;
  settingsDefaults();
  if (!oldOk) return;
  cfg.mode = old.mode == HOTSPOT ? HOTSPOT : HOME;   // "server" became "home"
  memcpy(cfg.apPassword, old.apPassword, sizeof cfg.apPassword);
  memcpy(cfg.wifiSsid, old.wifiSsid, sizeof cfg.wifiSsid);
  memcpy(cfg.wifiPassword, old.wifiPassword, sizeof cfg.wifiPassword);
  cfg.apPassword[63] = cfg.wifiSsid[32] = cfg.wifiPassword[63] = 0;
  settingsSave();
}

// ---- Link to the loupe side ----------------------------------------------------------------
static WebServerT web(80);
static DNSServer dns;
static frame::Reader reader;
static uint8_t linkBuf[frame::MAX_PAYLOAD + frame::OVERHEAD];

static String apSsid;
static bool apOn = false, staOn = false, fallback = false;
static uint32_t linkFull = 0, overruns = 0;          // overruns: bytes lost before the reader saw them

static void toLoupe(uint8_t type, const uint8_t* d, size_t n) {
  if (n > frame::MAX_PAYLOAD) return;
  size_t len = frame::build(type, d, n, linkBuf);
  if (LINK.availableForWrite() >= (int)len) LINK.write(linkBuf, len);
  else linkFull++;
}

// ---- One frame buffer ------------------------------------------------------------------------
// The next frame is only collected once the previous one was fetched. The browser's fetch rate thus
// paces the frame rate, and no second buffer is needed (the ESP8266 has too little RAM for two).
// When the loupe side announces the length (frame::LEN_MAGIC), the frame goes to the browser while it is
// still arriving: cable and WiFi then work at the same time instead of one after the other. Bytes already
// sent are dropped from the front of the buffer, so such a frame may be larger than the buffer.
// Measured: 5–33 KB per frame, depending on the subject. Larger frames stream through, so 20 KB is enough —
// and the 1460-byte lwIP needs the RAM: with 24 KB the free heap fell to 1.9 KB and the board hung.
static const size_t FRAME_MAX = 20 * 1024;
static uint8_t* img = nullptr;                       // allocated after WiFi is up (see setup)
static size_t imgCapacity = 0, imgLen = 0, imgExpect = 0;   // imgExpect: announced length, 0 = unknown
static size_t imgSent = 0, imgGone = 0;              // sent from the buffer / dropped from its front
static bool imgStreaming = false;                    // a browser is receiving this frame right now
static bool browserWaiting = false;                  // a frame request is open (waiting or receiving)
static size_t lastSize = 0;                          // length of the last frame served
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
    if (imgStreaming) { imgState = EMPTY; return; }  // a new frame in the middle of a stream: give up
    imgState = FILLING; imgLen = 0; imgNo = no; imgTotal = total; imgNext = 0; imgExpect = 0;
    imgSent = imgGone = 0;
    memcpy(&imgAngle, d + 3, 4);
    if (!memcmp(d + frame::LEN_AT, frame::LEN_MAGIC, 4)) {
      uint32_t len;
      memcpy(&len, d + frame::LEN_AT + 4, 4);
      imgExpect = len;
      // Asked for ahead and too big to wait in the buffer: drop it now, and stop asking ahead (grantFrames)
      // until frames get smaller. With a browser waiting, it streams through whatever its size.
      if (!browserWaiting && len + 2048 > imgCapacity) { imgState = EMPTY; lastSize = len; return; }
    }
  }
  if (imgState != FILLING || no != imgNo || seq != imgNext) { imgState = EMPTY; return; }
  size_t part = n - 24;
  if (img && imgLen + part > imgCapacity && imgSent) {   // make room: drop what the browser already has
    memmove(img, img + imgSent, imgLen - imgSent);
    imgLen -= imgSent; imgGone += imgSent; imgSent = 0;
  }
  if (!img || imgLen + part > imgCapacity) { imgState = EMPTY; tooBig++; return; }
  memcpy(img + imgLen, d + 24, part);
  imgLen += part; imgNext++;
  if (imgNext == imgTotal) { imgState = READY; lastFrame = millis(); }
}

// A frame can go out once it is complete, or as soon as its length is known.
static bool servable() { return imgState == READY || (imgState == FILLING && imgExpect); }

static void readLink(uint32_t waitMs = 0) {
  uint32_t t = millis();
#if defined(ESP8266)
  if (LINK.hasOverrun()) overruns++;
#endif
  static uint8_t rx[256];
  do {
    // In blocks, at most ~2 KB per call: byte by byte, the reader kept the CPU from the WiFi stack at 2 Mbaud.
    for (size_t budget = 2048; budget;) {
      int a = LINK.available();
      if (a <= 0) break;
      size_t k = LINK.read(rx, (size_t)a < sizeof rx ? (size_t)a : sizeof rx);
      if (!k) break;
      budget -= k < budget ? k : budget;
      for (size_t pos = 0, used; pos < k; pos += used) {
        if (!reader.feed(rx + pos, k - pos, used)) continue;
        if (reader.type == frame::VIDEO) {
          frameChunk(reader.data, reader.len);
        } else if (reader.type == frame::CONTROL) {
          int b = -1;
          for (int i = 0; i < reader.len; i++) if (reader.data[i] == '{') { b = i; break; }
          if (b >= 0) { telemetry = ""; telemetry.concat((const char*)reader.data + b, reader.len - b); }
        } else if (reader.type == frame::STATUS) {
          sideA = "";
          sideA.concat((const char*)reader.data, reader.len);   // payload is not NUL-terminated
          lastSideA = millis();
        }
      }
    }
    if (waitMs && !servable()) yield();
  } while (waitMs && !servable() && millis() - t < waitMs);
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

// Writes everything or gives up after 4 s without progress. With little free heap the TCP stack accepts only part of
// a large write; a single write() call then silently cut pages and frames short (measured: 13 464 of 17 391 bytes).
static bool writeAll(const uint8_t* d, size_t n) {
  WiFiClient c = web.client();
  uint8_t buf[1024];
  size_t off = 0;
  for (uint32_t t = millis(); off < n && c.connected() && millis() - t < 4000;) {
    size_t chunk = n - off < sizeof buf ? n - off : sizeof buf;
    memcpy_P(buf, d + off, chunk);                  // works for flash (PROGMEM) and RAM alike
    size_t w = c.write(buf, chunk);
    if (w) { off += w; t = millis(); } else { delay(2); }
  }
  return off == n;
}

static void sendPage() {
  lastViewer = millis();
  web.sendHeader("Content-Encoding", "gzip");
  web.sendHeader("Cache-Control", "no-cache");
  web.setContentLength(PAGE_GZ_LEN);
  web.send(200, "text/html; charset=utf-8", "");
  writeAll(PAGE_GZ, PAGE_GZ_LEN);
}

// Why the board last restarted, same words as the loupe side ("crash" and "watchdog" must never show up).
static const char* resetReason() {
#if defined(ESP8266)
  switch (ESP.getResetInfoPtr()->reason) {
    case REASON_DEFAULT_RST: case REASON_EXT_SYS_RST: return "power_on";
    case REASON_EXCEPTION_RST: return "crash";
    case REASON_WDT_RST: case REASON_SOFT_WDT_RST: return "watchdog";
    case REASON_SOFT_RESTART: return "software";
    default: return "other";
  }
#else
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_SW: return "software";
    default: return "other";
  }
#endif
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
  j += ",\"model_url\":" + jsonString(cfg.modelUrl) + ",\"model_name\":" + jsonString(cfg.modelName);
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
  j += ",\"link\":{\"a\":" + (aFresh ? sideA : String("null"));
  j += ",\"b\":{\"rssi\":" + String(staOn ? WiFi.RSSI() : 0) + ",\"crc\":" + reader.crc_errors +
       ",\"heap\":" + ESP.getFreeHeap() + ",\"served\":" + framesServed + ",\"too_big\":" + tooBig +
       ",\"buffer\":" + imgCapacity + ",\"link_full\":" + linkFull + ",\"overruns\":" + overruns +
       ",\"uptime\":" + millis() / 1000 + ",\"reset\":\"" + resetReason() + "\"}}}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

static void grantFrames(bool asked = false);
static uint32_t lastGrant = 0;

static void sendFrame() {
  lastViewer = millis();
  browserWaiting = true;
  grantFrames(true);
  readLink(800);                                    // wait for the next frame (complete, or its length known)
  if (!servable()) { browserWaiting = false; web.send(503, "text/plain", "no frame"); return; }
  size_t total = imgState == READY ? imgLen : imgExpect;
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("X-Angle", String(imgAngle, 1));
  web.sendHeader("Access-Control-Expose-Headers", "X-Angle");
  web.setContentLength(total);
  web.send(200, "image/jpeg", "");
  // Send what has arrived, keep reading the cable in between. Only as much as the TCP buffer takes, so the
  // UART is never left alone for long (its RX buffer holds ~40 ms at 2 Mbaud).
  WiFiClient c = web.client();
  c.setNoDelay(true);   // small pieces: without this Nagle waits for the browser's delayed ACK (measured 80 KB/s)
  imgStreaming = true;
  for (uint32_t t = millis(); imgGone + imgSent < total && c.connected() && millis() - t < 3000;) {
    readLink();
    if (imgState == EMPTY || (imgState == READY && imgGone + imgLen != total)) break;   // broken on the cable
    size_t n = imgLen - imgSent;
    // Full TCP segments only, except the last piece: many small segments fill the ESP8266's short send queue,
    // and the receiver acknowledges a lone small segment only after its delayed-ACK timer (measured: ~1 KB / 40 ms).
    if (n < 1460 && imgGone + imgLen < total) n = 0;
#if defined(ESP8266)
    size_t room = c.availableForWrite();
    if (n > room) n = room;
#endif
    if (n > 1460) n = 1460;
    size_t w = n ? c.write(img + imgSent, n) : 0;
    if (w) { imgSent += w; t = millis(); }
    yield();                                        // lets the WiFi stack take in ACKs and send
  }
  imgStreaming = browserWaiting = false;
  if (imgGone + imgSent == total) framesServed++;
  else c.stop();                                    // the browser sees a short image and asks again
  imgState = EMPTY;                                 // only now collect the next frame
  lastGrant = 0;                                    // and ask for it right away
  lastSize = total;
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
  j += ",\"model_url\":" + jsonString(cfg.modelUrl) + ",\"model_name\":" + jsonString(cfg.modelName);
  j += ",\"hostname\":\""; j += HOSTNAME; j += ".local\"}";
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

static void error(const char* code) { web.send(400, "application/json", String("{\"ok\":false,\"error\":\"") + code + "\"}"); }

static void saveSettings() {
  String mode = web.arg("mode"), ssid = web.arg("wifi_ssid"), pass = web.arg("wifi_password"), ap = web.arg("ap_password");
  String murl = web.arg("model_url"), mname = web.arg("model_name");
  murl.trim(); mname.trim();
  while (murl.endsWith("/")) murl.remove(murl.length() - 1);
  int m = mode == "home" ? HOME : mode == "hotspot" ? HOTSPOT : -1;
  if (m < 0) return error("mode");
  if (m != HOTSPOT && ssid.isEmpty() && !cfg.wifiSsid[0]) return error("wifi_ssid");
  if (ssid.length() > 32 || pass.length() > 63) return error("wifi_length");
  if (ap.length() && (ap.length() < 8 || ap.length() > 63)) return error("ap_password");
  if (murl.length() && (murl.length() > 95 || !(murl.startsWith("http://") || murl.startsWith("https://"))))
    return error("model_url");
  if (mname.length() > 63) return error("model_name");
  cfg.mode = m;
  if (ssid.length()) {
    if (ssid != cfg.wifiSsid) memset(cfg.wifiPassword, 0, sizeof cfg.wifiPassword);   // new network, new password
    strncpy(cfg.wifiSsid, ssid.c_str(), sizeof cfg.wifiSsid - 1);
  }
  if (pass.length()) strncpy(cfg.wifiPassword, pass.c_str(), sizeof cfg.wifiPassword - 1);
  if (ap.length()) { memset(cfg.apPassword, 0, sizeof cfg.apPassword); strncpy(cfg.apPassword, ap.c_str(), sizeof cfg.apPassword - 1); }
  // The model fields are always sent by the page; empty clears them.
  if (web.hasArg("model_url")) { memset(cfg.modelUrl, 0, sizeof cfg.modelUrl); strncpy(cfg.modelUrl, murl.c_str(), sizeof cfg.modelUrl - 1); }
  if (web.hasArg("model_name")) { memset(cfg.modelName, 0, sizeof cfg.modelName); strncpy(cfg.modelName, mname.c_str(), sizeof cfg.modelName - 1); }
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

// ---- Flow control: ask the loupe side for one whole frame when the buffer is empty ----------
// (again after 400 ms if it got lost on the way). Asking ahead, before the browser does, only pays off
// while frames fit the buffer: a large frame arriving with nobody to pass it on to overflows it.
static void grantFrames(bool asked) {
  uint32_t now = millis();
  if (!viewerActive() || imgState != EMPTY) return;
  if (!asked && (now - lastGrant < 400 || lastSize + 2048 > imgCapacity)) return;
  uint8_t n = 1;
  toLoupe(frame::READY, &n, 1);
  lastGrant = now;
}

// ---- Keep the loupe awake while somebody watches --------------------------------------------
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
    for (int i = 0; i < 3; i++) toLoupe(frame::VIDEO, VIDEO_STOP, sizeof VIDEO_STOP);
    wasActive = false; imgState = EMPTY;
  }
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
  readLink();
  grantFrames();
  web.handleClient();
#if defined(ESP8266)
  if (staOn) MDNS.update();
#endif
  localTick();
  staOn = cfg.mode != HOTSPOT && WiFi.status() == WL_CONNECTED;
  bool lonely = !staOn && !(apOn && WiFi.softAPgetStationNum() > 0);
  bool on = lonely ? (millis() / 300) % 2 : viewerActive();
  digitalWrite(PIN_LED, on ? LED_ON : !LED_ON);
  yield();
}
