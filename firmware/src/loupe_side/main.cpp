// Loupe side (ESP32): wakes the WiFi loupe over Bluetooth LE, joins the loupe's own WiFi and passes
// its UDP traffic over the UART cable to the home side (and back). Protocol: docs/protocol.md.
//
// Every video frame is assembled here completely and only forwarded when whole — the home side can then
// stream it without its own buffer, and a server never sees fragments. If the previous frame is still on the
// cable when the next one is complete, the next one is dropped as a whole: fewer frames beat striped frames.
//
// Flow control: a frame only goes over the cable when the home side has granted one (READY). The loupe delivers
// ~16 frames/s of up to 21 KB; sent non-stop at 2 Mbaud, the ESP8266 could not keep up next to its WiFi, its
// 128-byte hardware FIFO overflowed and nearly every frame arrived broken (measured: 1310 CRC errors, 0 frames).
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
// Fixed buffers, not String: the connection task (core 0) writes them while the status (core 1) reads them;
// a String reallocating under the reader's feet can crash. A torn name for one status line is harmless.
static char ssid[33] = "", password[13] = "";

static AsyncUDP udpVideo, udpControl;
static frame::Reader reader;
static uint8_t sendBuf[frame::MAX_PAYLOAD + frame::OVERHEAD];
static SemaphoreHandle_t uartLock;
static uint32_t framesSent = 0, framesDropped = 0, framesAborted = 0, framesSkipped = 0, lastPacket = 0;
static volatile int credits = 0;                     // frames the home side has asked for

static bool linkSend(uint8_t type, const uint8_t* d, size_t len) {
  if (len > frame::MAX_PAYLOAD) return false;
  xSemaphoreTake(uartLock, portMAX_DELAY);
  bool ok = Serial2.availableForWrite() >= (int)(len + frame::OVERHEAD);
  if (ok) Serial2.write(sendBuf, frame::build(type, d, len, sendBuf));
  xSemaphoreGive(uartLock);
  return ok;
}

// ---- Frame assembly ------------------------------------------------------------------------
// One buffer of slots; every chunk lands in its slot already in link-frame layout, so a complete frame goes
// onto the cable without another copy. While a frame is on the cable, new ones are dropped — two buffers of
// 64 KB left too little RAM for WiFi and Bluetooth (boot loop: esp_nimble_hci_init ESP_ERR_NO_MEM).
static const int MAX_CHUNKS = 40;                  // 40 × 1607 B ≈ 64 KB; a frame has ~5–16 chunks
static const size_t SLOT = frame::MAX_PAYLOAD + frame::OVERHEAD;
static uint8_t* slots;
static uint16_t chunkLen[MAX_CHUNKS];
static int frameNo = -1, frameTotal = 0, frameHave = 0;
static uint64_t frameMask = 0;
static volatile int outCount = 0;                  // > 0: slots 0 … outCount-1 are waiting for the cable
static int outIndex = 0;

static void frameReset() { frameNo = -1; frameTotal = 0; frameHave = 0; frameMask = 0; }

static void onVideo(AsyncUDPPacket& p) {
  lastPacket = millis();
  const uint8_t* d = p.data();
  size_t n = p.length();
  if (n < 24 || n > frame::MAX_PAYLOAD) { linkSend(frame::VIDEO, d, n); return; }
  if (outCount) {                                   // the previous frame is still on the cable
    if (d[1] == 0) framesAborted++;
    frameReset();
    return;
  }
  int no = d[0], seq = d[1], total = d[2];
  if (no != frameNo) {                              // a new frame: an incomplete old one expires
    if (frameNo >= 0 && frameHave > 0) framesDropped++;
    frameReset();
    if (total <= 0 || total > MAX_CHUNKS) { framesDropped++; return; }
    frameNo = no; frameTotal = total;
  }
  if (seq >= frameTotal || (frameMask >> seq) & 1) return;
  memcpy(slots + seq * SLOT + 5, d, n); chunkLen[seq] = n;
  frameMask |= (uint64_t)1 << seq; frameHave++;
  if (frameHave < frameTotal) return;
  if (credits <= 0) { framesSkipped++; frameReset(); return; }   // nobody asked for it
  for (int i = 0; i < frameTotal; i++) frame::seal(frame::VIDEO, chunkLen[i], slots + i * SLOT);
  outIndex = 0;
  credits--;
  framesSent++;
  outCount = frameTotal;                            // set last: loop() starts sending now
  frameReset();
}

// Writes the queued link frames, each one whole — never a part, so status and control frames can go in between.
// availableForWrite() of the ESP32 UART driver reports the largest contiguous free piece of a no-split ring
// buffer (at most half of it), so checking room for a whole 20 KB frame at once never succeeded.
static void drainOut() {
  while (outCount && outIndex < outCount) {
    size_t len = chunkLen[outIndex] + frame::OVERHEAD;
    xSemaphoreTake(uartLock, portMAX_DELAY);
    bool ok = Serial2.availableForWrite() >= (int)len;
    if (ok) Serial2.write(slots + outIndex * SLOT, len);
    xSemaphoreGive(uartLock);
    if (!ok) return;
    outIndex++;
  }
  outCount = 0;
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
    snprintf(ssid, sizeof ssid, "%s", dev.getName().c_str());
    for (int k = 0; k < 6; k++) snprintf(password + 2 * k, 3, "%02x", (uint8_t)m[8 + k]);
    address = dev.getAddress();
    found = true;
  }
  scan->stop();
  scan->clearResults();
  if (found) {
    state = WAKE;
    Serial.printf("Found loupe %s, waking it\n", ssid);
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

// An already awake loupe may not advertise over BLE any more, but its WiFi is visible — and the password is the
// BSSID, which the scan shows anyway. So look for it directly first.
static bool findAwakeLoupe() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n && !found; i++) {
    if (!WiFi.SSID(i).startsWith("softish-")) continue;
    const uint8_t* b = WiFi.BSSID(i);
    snprintf(ssid, sizeof ssid, "%s", WiFi.SSID(i).c_str());
    for (int k = 0; k < 6; k++) snprintf(password + 2 * k, 3, "%02x", b[k]);
    found = true;
    Serial.printf("Loupe %s is awake already\n", ssid);
  }
  WiFi.scanDelete();
  return found;
}

static bool joinLoupeWifi() {
  state = JOIN;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  for (int attempt = 0; attempt < 3; attempt++) {
    WiFi.begin(ssid, password);
    for (uint32_t t = millis(); millis() - t < 10000;) {
      if (WiFi.status() == WL_CONNECTED) return true;
      delay(100);
    }
    WiFi.disconnect(true);
    delay(500);
  }
  return false;
}

// Why the board last restarted — "brownout" means the power supply sags (WiFi + BLE draw current peaks).
static const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_SW: return "software";
    default: return "other";
  }
}

static void sendStatus() {
  static const char* names[] = {"search", "wake", "join", "connected"};
  char name[33];
  memcpy(name, ssid, sizeof name - 1);
  name[32] = 0;
  char json[320];
  int n = snprintf(json, sizeof json,
                   "{\"state\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"frames\":%u,\"dropped\":%u,"
                   "\"aborted\":%u,\"skipped\":%u,\"crc\":%u,\"uptime\":%lu,\"reset\":\"%s\"}",
                   names[state], name, state == CONNECTED ? WiFi.RSSI() : 0, framesSent, framesDropped,
                   framesAborted, framesSkipped, reader.crc_errors, millis() / 1000, resetReason());
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
    if (!findAwakeLoupe() && !findAndWake()) { delay(1000); continue; }
    if (!joinLoupeWifi()) continue;
    udpVideo.listen(PORT_VIDEO);
    udpVideo.onPacket(onVideo);
    udpControl.listen(PORT_CONTROL);
    udpControl.onPacket(onControl);
    Serial.printf("Connected to %s, IP %s\n", ssid, WiFi.localIP().toString().c_str());
    state = CONNECTED;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  uartLock = xSemaphoreCreateMutex();
  slots = (uint8_t*)malloc(MAX_CHUNKS * SLOT);
  Serial2.setTxBufferSize(TX_BUFFER);
  Serial2.setRxBufferSize(4096);
  Serial2.begin(LINK_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);
  xTaskCreatePinnedToCore(connectionTask, "connection", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  // Commands from the home side (start, heartbeat, light) go on to the loupe.
  while (Serial2.available()) {
    if (!reader.feed(Serial2.read())) continue;
    if (reader.type == frame::READY && reader.len == 1) { credits = reader.data[0]; continue; }
    if (state != CONNECTED) continue;
    if (reader.type == frame::VIDEO) udpVideo.writeTo(reader.data, reader.len, LOUPE, PORT_VIDEO);
    if (reader.type == frame::CONTROL) udpControl.writeTo(reader.data, reader.len, LOUPE, PORT_CONTROL);
  }
  drainOut();
  static uint32_t lastStatus = 0;
  if (millis() - lastStatus >= 1000) {
    lastStatus = millis();
    sendStatus();
  }
  uint32_t blink = state == SEARCH ? 500 : 100;
  digitalWrite(PIN_LED, state == CONNECTED && millis() - lastPacket < 2000 ? HIGH : (millis() / blink) % 2);
  delay(1);
}
