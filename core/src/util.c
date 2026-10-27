#include "util.h"

uint32_t crc32_update(uint32_t crc, const void *p, size_t n) {
  static const uint32_t nib[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  const uint8_t *b = p;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= b[i];
    crc = (crc >> 4) ^ nib[crc & 15];
    crc = (crc >> 4) ^ nib[crc & 15];
  }
  return ~crc;
}

uint32_t crc32_of(const void *p, size_t n) { return crc32_update(0, p, n); }

void sb_init(SBuf *s, char *p, size_t cap) {
  s->p = p; s->cap = cap; s->len = 0; s->ovf = 0;
  if (cap) p[0] = 0;
}

void sb_ch(SBuf *s, char c) {
  if (s->len + 1 >= s->cap) { s->ovf = 1; return; }
  s->p[s->len++] = c;
  s->p[s->len] = 0;
}

void sb_str(SBuf *s, const char *z) { while (*z) sb_ch(s, *z++); }

void sb_u64(SBuf *s, uint64_t v) {
  char t[24]; int i = 0;
  do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (i) sb_ch(s, t[--i]);
}

void sb_i64(SBuf *s, int64_t v) {
  if (v < 0) { sb_ch(s, '-'); sb_u64(s, 0 - (uint64_t)v); }
  else sb_u64(s, (uint64_t)v);
}

void sb_hex(SBuf *s, uint64_t v, int digits) {
  for (int i = digits - 1; i >= 0; i--) sb_ch(s, "0123456789abcdef"[(v >> (4 * i)) & 15]);
}

void sb_hexbytes(SBuf *s, const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) sb_hex(s, b[i], 2);
}
