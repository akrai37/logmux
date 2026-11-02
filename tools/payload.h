#ifndef LOGMUX_TOOLS_PAYLOAD_H
#define LOGMUX_TOOLS_PAYLOAD_H

// The lines logmux-child writes, generated from (seed, child, sequence
// number) alone, so the verifier can regenerate every expected line and
// compare the log against it byte for byte.
//
//   c<child> s<seq> <payload>
//
// Payload lengths vary from empty to `max_len`, so some lines exceed a
// collector's max_line and must come out in pieces.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static inline uint64_t lm_mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

static inline uint64_t lm_line_key(uint64_t seed, uint32_t child, uint64_t seq) {
  return lm_mix(seed ^ ((uint64_t)child << 40) ^ lm_mix(seq));
}

// Which stream line `seq` goes to when a child writes to both (0 = stdout).
static inline int lm_line_stream(uint64_t seed, uint32_t child, uint64_t seq) {
  return (int)(lm_line_key(seed, child, seq) >> 63);
}

// Writes line `seq` of `child` (without the newline) into `out`, which must
// hold at least lm_line_capacity(max_len) bytes. Returns its length.
static inline size_t lm_line_capacity(size_t max_len) {
  return max_len + 48;
}

static inline size_t lm_make_line(char* out, uint64_t seed, uint32_t child, uint64_t seq,
                                  size_t max_len) {
  uint64_t key = lm_line_key(seed, child, seq);
  // Mostly short lines, sometimes up to max_len.
  const size_t len = (key & 7) == 0 ? (size_t)((key >> 8) % (max_len + 1))
                                    : (size_t)((key >> 8) % (max_len / 16 + 1));
  int n = snprintf(out, 48, "c%u s%llu ", child, (unsigned long long)seq);
  size_t pos = n > 0 ? (size_t)n : 0;
  for (size_t i = 0; i < len; ++i) {
    if (i % 8 == 0) {
      key = lm_mix(key);
    }
    out[pos++] = (char)('a' + (char)((key >> (8 * (i % 8))) % 26));
  }
  return pos;
}

#endif
