#include <cstring>
// Host test for frame.h:  g++ -std=c++17 -Iinclude test/frame_test.cpp -o /tmp/frame_test && /tmp/frame_test
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#include "frame.h"

int main() {
  uint8_t data[1400], buf[1500];
  for (int i = 0; i < 1400; i++) data[i] = rand();
  std::vector<uint8_t> stream = {0x00, 0xA5, 0x13, 0xA5};  // junk first, including half a magic
  size_t n = frame::build(frame::VIDEO, data, 1400, buf);
  stream.insert(stream.end(), buf, buf + n);
  n = frame::build(frame::STATUS, (const uint8_t*)"{}", 2, buf);
  stream.insert(stream.end(), buf, buf + n);

  frame::Reader r;
  int done = 0;
  for (uint8_t b : stream)
    if (r.feed(b)) {
      if (done == 0) assert(r.type == frame::VIDEO && r.len == 1400 && !memcmp(r.data, data, 1400));
      if (done == 1) assert(r.type == frame::STATUS && r.len == 2 && r.data[0] == '{');
      done++;
    }
  assert(done == 2);

  // Failure case: one flipped bit must not deliver a frame; the next one must arrive again.
  n = frame::build(frame::CONTROL, data, 100, buf);
  buf[50] ^= 0x04;
  for (size_t i = 0; i < n; i++) assert(!r.feed(buf[i]));
  assert(r.crc_errors == 1);
  n = frame::build(frame::CONTROL, data, 100, buf);
  bool ok = false;
  for (size_t i = 0; i < n; i++) ok |= r.feed(buf[i]);
  assert(ok && r.type == frame::CONTROL);

  // Failure case: a length above the maximum is dropped instead of overflowing the buffer.
  uint8_t evil[] = {0xA5, 0x5A, 1, 0xFF, 0xFF};
  for (uint8_t b : evil) assert(!r.feed(b));
  assert(r.too_long == 1);
  // seal(): a payload already in place gives exactly the same bytes as build().
  uint8_t sealed[1500], built[1500];
  memcpy(sealed + 5, data, 700);
  size_t ns = frame::seal(frame::VIDEO, 700, sealed), nb = frame::build(frame::VIDEO, data, 700, built);
  assert(ns == nb && memcmp(sealed, built, nb) == 0);
  uint8_t one = 1;
  n = frame::build(frame::READY, &one, 1, buf);
  ok = false;
  for (size_t i = 0; i < n; i++) ok |= r.feed(buf[i]);
  assert(ok && r.type == frame::READY && r.len == 1 && r.data[0] == 1);
  // crc16() with its table must equal the plain bitwise CRC-16/CCITT (any start value, any length).
  auto bitwise = [](const uint8_t* d, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; i++) {
      crc ^= (uint16_t)d[i] << 8;
      for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
  };
  for (int len : {0, 1, 3, 9, 700, 1400}) assert(frame::crc16(data, len) == bitwise(data, len, 0xFFFF));
  assert(frame::crc16(data, 50, 0x1D0F) == bitwise(data, 50, 0x1D0F));
  assert(frame::crc16((const uint8_t*)"123456789", 9) == 0x29B1);   // published check value of CRC-16/CCITT-FALSE

  // Block feed: the same stream, cut into blocks of every size, gives the same frames as byte by byte.
  for (size_t block : {1, 7, 64, 256, 1500, 5000}) {
    frame::Reader rb;
    int frames = 0;
    for (size_t off = 0; off < stream.size(); off += block) {
      size_t k = stream.size() - off < block ? stream.size() - off : block, pos = 0, used;
      while (pos < k) {
        if (rb.feed(stream.data() + off + pos, k - pos, used)) {
          if (frames == 0) assert(rb.type == frame::VIDEO && rb.len == 1400 && !memcmp(rb.data, data, 1400));
          if (frames == 1) assert(rb.type == frame::STATUS && rb.len == 2);
          frames++;
        }
        pos += used;
      }
    }
    assert(frames == 2 && rb.crc_errors == 0);
  }
  printf("frame: all cases passed (good=%u, crc_errors=%u, too_long=%u)\n", r.good, r.crc_errors, r.too_long);
  return 0;
}
