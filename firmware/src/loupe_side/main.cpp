// Loupe side (ESP32): wakes the WiFi loupe over Bluetooth LE, joins the loupe's own WiFi and passes
// its UDP traffic over the UART cable to the home side (and back). Protocol: docs/protocol.md.
//
// Every video frame is assembled here completely and only forwarded when whole — the home side can then
// stream it without its own buffer, and a server never sees fragments. If a frame does not fit into the
// UART send buffer, it is dropped as a whole: fewer frames beat striped frames.
//
// LED (GPIO 2): slow blink = searching for the loupe, fast blink = joining its WiFi, on = video flowing.
#include <Arduino.h>
#include <AsyncUDP.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include "frame.h"

#ifndef LINK_BAUD
#define LINK_BAUD 2000000
#endif
#ifndef PIN_RX
#define PIN_RX 16   // labelled RX2 / D16
#endif
#ifndef PIN_TX
#define PIN_TX 17   // labelled TX2 / D17
#endif
#ifndef PIN_LED
#define PIN_LED 2
#endif

static const uint16_t COMPANY_ID = 0x270F;   // BLE manufacturer data of the loupe
static const uint16_t PORT_VIDEO = 61501, PORT_CONTROL = 61500;
static const size_t TX_BUFFER = 32768;
static const IPAddress LOUPE(192, 168, 1, 1);

enum State { SEARCH, WAKE, JOIN, CONNECTED };
static volatile State state = SEARCH;
static String ssid, password;

static AsyncUDP udpVideo, udpControl;
static frame::Reader reader;
static uint8_t sendBuf[frame::MAX_PAYLOAD + frame::OVERHEAD];
static SemaphoreHandle_t uartLock;
static uint32_t framesSent = 0, framesDropped = 0, framesAborted = 0, lastPacket = 0;

static bool linkSend(uint8_t type, const uint8_t* d, size_t len) {
  if (len > frame::MAX_PAYLOAD) return false;
  xSemaphoreTake(uartLock, portMAX_DELAY);
  bool ok = Serial2.availableForWrite() >= (int)(len + frame::OVERHEAD);
  if (ok) Serial2.write(sendBuf, frame::build(type, d, len, sendBuf));
  xSemaphoreGive(uartLock);
  return ok;
}

// ---- Frame assembly ------------------------------------------------------------------------
static const int MAX_CHUNKS = 40;                  // 40 × 1600 B = 64 KB; a frame has ~5–16 chunks
static uint8_t* chunk[MAX_CHUNKS];
static uint16_t chunkLen[MAX_CHUNKS];
static int frameNo = -1, frameTotal = 0, frameHave = 0;
static uint64_t frameMask = 0;

static void frameReset() { frameNo = -1; frameTotal = 0; frameHave = 0; frameMask = 0; }

static void onVideo(AsyncUDPPacket& p) {
  lastPacket = millis();
  const uint8_t* d = p.data();
  size_t n = p.length();
  if (n < 24 || n > frame::MAX_PAYLOAD) { linkSend(frame::VIDEO, d, n); return; }
  int no = d[0], seq = d[1], total = d[2];
  if (no != frameNo) {                              // a new frame: an incomplete old one expires
    if (frameNo >= 0 && frameHave > 0) framesDropped++;
    frameReset();
    if (total <= 0 || total > MAX_CHUNKS) { framesDropped++; return; }
    frameNo = no; frameTotal = total;
  }
  if (seq >= frameTotal || (frameMask >> seq) & 1) return;
  memcpy(chunk[seq], d, n); chunkLen[seq] = n;
  frameMask |= (uint64_t)1 << seq; frameHave++;
  if (frameHave < frameTotal) return;
  // complete: all or nothing over the cable
  size_t needed = 0;
  for (int i = 0; i < frameTotal; i++) needed += chunkLen[i] + frame::OVERHEAD;
  xSemaphoreTake(uartLock, portMAX_DELAY);
  bool fits = Serial2.availableForWrite() >= (int)needed;
  if (fits)
    for (int i = 0; i < frameTotal; i++) Serial2.write(sendBuf, frame::build(frame::VIDEO, chunk[i], chunkLen[i], sendBuf));
  xSemaphoreGive(uartLock);
  if (fits) framesSent++; else framesAborted++;
  frameReset();
}

static void onControl(AsyncUDPPacket& p) { linkSend(frame::CONTROL, p.data(), p.length()); }

// ---- Find and wake the loupe -----------------------------------------------------------------
static bool findAndWake() {
  state = SEARCH;
  // Initialise once, never deinit: after NimBLEDevice::deinit(true) a pending NimBLE timer fired into
  // freed memory — crash "InstrFetchProhibited" in os_callout_timer_cb, endless reboot loop.
  static bool bleReady = false;
  if (!bleReady) { NimBLEDevice::init(""); bleReady = true; }
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults hits = scan->start(5, false);
  NimBLEAddress address;
  bool found = false;
  for (int i = 0; i < hits.getCount() && !found; i++) {
    NimBLEAdvertisedDevice dev = hits.getDevice(i);
    if (!dev.haveManufacturerData() || !dev.haveName()) continue;
    std::string m = dev.getManufacturerData();     // starts with the company ID, little-endian
    if (m.size() < 14 || (uint8_t)m[0] != (COMPANY_ID & 0xFF) || (uint8_t)m[1] != (COMPANY_ID >> 8)) continue;
    // WiFi name = BLE name; WiFi password = the BSSID (bytes 6–11 of the payload) as 12 lowercase hex digits
    ssid = dev.getName().c_str();
    password = "";
    for (int k = 8; k < 14; k++) { char h[3]; snprintf(h, 3, "%02x", (uint8_t)m[k]); password += h; }
    address = dev.getAddress();
    found = true;
  }
  scan->stop();
  scan->clearResults();
  if (found) {
    state = WAKE;
    Serial.printf("Found loupe %s, waking it\n", ssid.c_str());
    NimBLEClient* c = NimBLEDevice::createClient();
    // The read itself is the wake-up call; the value does not matter.
    if (c->connect(address)) {
      NimBLERemoteService* s = c->getService(NimBLEUUID((uint16_t)0x9900));
      NimBLERemoteCharacteristic* ch = s ? s->getCharacteristic(NimBLEUUID((uint16_t)0x0099)) : nullptr;
      if (ch) ch->readValue();
      c->disconnect();
    } else {
      found = false;
    }
    NimBLEDevice::deleteClient(c);
  }
  return found;
}

static bool joinLoupeWifi() {
  state = JOIN;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  for (int attempt = 0; attempt < 3; attempt++) {
    WiFi.begin(ssid.c_str(), password.c_str());
    for (uint32_t t = millis(); millis() - t < 10000;) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(100);
    }
    WiFi.disconnect(true);
    delay(500);
  }
  return false;
}

static void sendStatus() {
  static const char* names[] = {"search", "wake", "join", "connected"};
  char json[256];
  int n = snprintf(json, sizeof json,
                   "{\"state\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"frames\":%u,\"dropped\":%u,"
                   "\"aborted\":%u,\"crc\":%u,\"uptime\":%lu}",
                   names[state], ssid.c_str(), state == CONNECTED ? WiFi.RSSI() : 0, framesSent, framesDropped,
                   framesAborted, reader.crc_errors, millis() / 1000);
  linkSend(frame::STATUS, (const uint8_t*)json, n);
}

// Runs on its own core: searching and joining block, but the cable must keep being served.
static void connectionTask(void*) {
  for (;;) {
    if (state == CONNECTED) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Loupe WiFi lost, searching again");
        udpVideo.close();
        udpControl.close();
        WiFi.disconnect(true);
        state = SEARCH;
      }
      delay(500);
      continue;
    }
    if (!findAndWake()) { delay(1000); continue; }
    if (!joinLoupeWifi()) continue;
    udpVideo.listen(PORT_VIDEO);
    udpVideo.onPacket(onVideo);
    udpControl.listen(PORT_CONTROL);
    udpControl.onPacket(onControl);
    Serial.printf("Connected to %s, IP %s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
    state = CONNECTED;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  uartLock = xSemaphoreCreateMutex();
  for (int i = 0; i < MAX_CHUNKS; i++) chunk[i] = (uint8_t*)malloc(frame::MAX_PAYLOAD);
  Serial2.setTxBufferSize(TX_BUFFER);
  Serial2.setRxBufferSize(4096);
  Serial2.begin(LINK_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  xTaskCreatePinnedToCore(connectionTask, "connection", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  // Commands from the home side (start, heartbeat, light) go on to the loupe.
  while (Serial2.available()) {
    if (!reader.feed(Serial2.read())) continue;
    if (state != CONNECTED) continue;
    if (reader.type == frame::VIDEO) udpVideo.writeTo(reader.data, reader.len, LOUPE, PORT_VIDEO);
    if (reader.type == frame::CONTROL) udpControl.writeTo(reader.data, reader.len, LOUPE, PORT_CONTROL);
  }
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= 1000) {
    lastStatus = millis();
    sendStatus();
  }
  uint32_t blink = state == SEARCH ? 500 : 100;
  digitalWrite(PIN_LED, state == CONNECTED && millis() - lastPacket < 2000 ? HIGH : (millis() / blink) % 2);
  delay(1);
}
