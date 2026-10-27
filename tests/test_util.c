#include "t.h"
#include "util.h"

static void crc_vectors(void) {
  CHECK_EQ(crc32_of("123456789", 9), 0xcbf43926u);
  CHECK_EQ(crc32_of("", 0), 0);
  uint32_t part = crc32_update(crc32_update(0, "1234", 4), "56789", 5);
  CHECK_EQ(part, 0xcbf43926u);
}

static void sbuf_numbers(void) {
  char b[64];
  SBuf s;
  sb_init(&s, b, sizeof b);
  sb_i64(&s, INT64_MIN); sb_ch(&s, ' '); sb_i64(&s, -1); sb_ch(&s, ' '); sb_u64(&s, UINT64_MAX); sb_ch(&s, ' '); sb_u64(&s, 0);
  CHECK(!s.ovf);
  CHECK_STR(b, "-9223372036854775808 -1 18446744073709551615 0");
  sb_init(&s, b, sizeof b);
  sb_hex(&s, 0xabc, 8); sb_hexbytes(&s, (const uint8_t *)"\x01\xff", 2);
  CHECK_STR(b, "00000abc01ff");
}

static void sbuf_overflow(void) {
  char b[8];
  SBuf s;
  sb_init(&s, b, sizeof b);
  sb_str(&s, "0123456789");
  CHECK(s.ovf);
  CHECK_EQ(strlen(b), 7);
  sb_init(&s, b, 0);
  sb_str(&s, "x");
  CHECK(s.ovf);
}

int main(void) {
  RUN(crc_vectors);
  RUN(sbuf_numbers);
  RUN(sbuf_overflow);
  return t_done("util");
}
