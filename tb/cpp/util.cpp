// SPDX-License-Identifier: MIT

#include "util.h"

#include <cstdio>

namespace wtb {

std::string hex_dump(const Bytes& b, size_t max_bytes) {
  std::string s;
  char buf[8];
  size_t n = b.size() < max_bytes ? b.size() : max_bytes;
  for (size_t i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%02x", b[i]);
    if (i) s += ' ';
    s += buf;
  }
  if (n < b.size()) {
    snprintf(buf, sizeof(buf), " ...");
    s += buf;
    snprintf(buf, sizeof(buf), "(%zu)", b.size());
    s += buf;
  }
  return s;
}

Bytes random_block(size_t len, uint32_t seed) {
  Bytes out(len);
  // xorshift32, chosen because it is four lines and the same in any language,
  // so a failure can be reproduced outside the testbench.
  uint32_t x = seed ? seed : 1;
  for (size_t i = 0; i < len; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    out[i] = uint8_t(x >> 24);
  }
  return out;
}

}  // namespace wtb
