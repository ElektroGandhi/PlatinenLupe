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
  printf("frame: all cases passed (good=%u, crc_errors=%u, too_long=%u)\n", r.good, r.crc_errors, r.too_long);
  return 0;
}
