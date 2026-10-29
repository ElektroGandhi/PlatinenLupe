// Framing for the UART link between the two boards (loupe side ↔ home side).
//
//   A5 5A | type u8 | length u16 LE | payload | CRC16-CCITT u16 LE (over type, length, payload)
//
// Plain C++ without Arduino so that test/frame_test.cpp can exercise it on a PC.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace frame {

enum Type : uint8_t {
  VIDEO = 1,    // UDP port 61501 of the loupe, both directions
  CONTROL = 2,  // UDP port 61500 of the loupe, both directions
  STATUS = 3,   // loupe side → home side: JSON with the state of the loupe side
  READY = 4,    // home side → loupe side: 1 byte, how many whole frames it may send now (flow control)
};

constexpr uint8_t MAGIC0 = 0xA5, MAGIC1 = 0x5A;
constexpr size_t MAX_PAYLOAD = 1600;  // one UDP packet of the loupe is ~1.4 KB
constexpr size_t OVERHEAD = 7;        // 2 magic + 1 type + 2 length + 2 CRC

// On the cable only: the loupe side writes the JPEG length of the whole frame into bytes 16–23 of the
// 24-byte loupe header of chunk 0 ("PLEN" + u32 LE). The home side can then send Content-Length and pass
// the frame on to the browser while it is still arriving. Without the marker it waits for the whole frame.
constexpr size_t LEN_AT = 16;
constexpr uint8_t LEN_MAGIC[4] = {'P', 'L', 'E', 'N'};

// Four bits per step from a 16-entry table: the same result as the bitwise loop, a quarter of the work.
// At 2 Mbaud the ESP8266 checks ~200 KB/s next to its WiFi; the bitwise loop cost it noticeable time.
inline uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
  static const uint16_t T[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
                                 0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF};
  for (size_t i = 0; i < n; i++) {
    crc = (crc << 4) ^ T[(crc >> 12) ^ (d[i] >> 4)];
    crc = (crc << 4) ^ T[(crc >> 12) ^ (d[i] & 0x0F)];
  }
  return crc;
}

// Writes a frame to `out` (needs len + OVERHEAD bytes). Returns the total length.
inline size_t build(uint8_t type, const uint8_t* data, uint16_t len, uint8_t* out) {
  out[0] = MAGIC0; out[1] = MAGIC1; out[2] = type;
  out[3] = len & 0xFF; out[4] = len >> 8;
  memcpy(out + 5, data, len);
  uint16_t c = crc16(out + 2, 3 + len);
  out[5 + len] = c & 0xFF; out[6 + len] = c >> 8;
  return len + OVERHEAD;
}

// Like build(), for a payload that already sits at out + 5: writes only header and CRC (no copy).
inline size_t seal(uint8_t type, uint16_t len, uint8_t* out) {
  out[0] = MAGIC0; out[1] = MAGIC1; out[2] = type;
  out[3] = len & 0xFF; out[4] = len >> 8;
  uint16_t c = crc16(out + 2, 3 + len);
  out[5 + len] = c & 0xFF; out[6 + len] = c >> 8;
  return len + OVERHEAD;
}

// Byte-by-byte reader. On a broken frame it resynchronises on the next magic; nothing half is delivered.
class Reader {
 public:
  uint32_t crc_errors = 0, too_long = 0, good = 0;

  // true when this byte completes a frame with a valid CRC.
  bool feed(uint8_t b) {
    switch (state_) {
      case 0: if (b == MAGIC0) state_ = 1; return false;
      case 1: state_ = (b == MAGIC1) ? 2 : (b == MAGIC0 ? 1 : 0); return false;
      case 2: type = b; state_ = 3; return false;
      case 3: len = b; state_ = 4; return false;
      case 4:
        len |= (uint16_t)b << 8;
        if (len > MAX_PAYLOAD) { too_long++; state_ = 0; return false; }
        pos_ = 0; state_ = len ? 5 : 6; return false;
      case 5: data[pos_++] = b; if (pos_ == len) state_ = 6; return false;
      case 6: crc_lo_ = b; state_ = 7; return false;
      case 7: {
        state_ = 0;
        uint8_t head[3] = {type, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
        uint16_t c = crc16(data, len, crc16(head, 3));
        if (c != (uint16_t)(crc_lo_ | (b << 8))) { crc_errors++; return false; }
        good++;
        return true;
      }
    }
    state_ = 0;
    return false;
  }

  // Like feed(), for a block: stops after a completed frame (true) or when all n bytes are used.
  // `used` says how many bytes were taken. The payload is copied in one piece.
  bool feed(const uint8_t* p, size_t n, size_t& used) {
    used = 0;
    while (used < n) {
      if (state_ == 5) {
        size_t k = n - used < (size_t)(len - pos_) ? n - used : (size_t)(len - pos_);
        memcpy(data + pos_, p + used, k);
        pos_ += k; used += k;
        if (pos_ == len) state_ = 6;
        continue;
      }
      if (feed(p[used++])) return true;
    }
    return false;
  }

  uint8_t type = 0;
  uint16_t len = 0;
  uint8_t data[MAX_PAYLOAD];

 private:
  uint8_t state_ = 0, crc_lo_ = 0;
  uint16_t pos_ = 0;
};

}  // namespace frame
