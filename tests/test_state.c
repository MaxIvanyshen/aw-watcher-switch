#include <errno.h>

#include "fakes/fake_fs.h"
#include "paths.h"
#include "state.h"
#include "t.h"
#include "util.h"

static FakeFs F;

static PState sample(void) {
  PState s;
  memset(&s, 0, sizeof s);
  s.ep = 3; s.cursor = 12345; s.anchor = 0xdeadbeef; s.anchor_ok = 1;
  s.tr.open = 1; s.tr.pol = 1; s.tr.prev_valid = 1;
  s.tr.pid = 0x0100000000001234ull; s.tr.su = 11; s.tr.sn = 22; s.tr.ss = 33; s.tr.alive = 44; s.tr.prev_steady = 55;
  s.src_ok = 1;
  for (int i = 0; i < 16; i++) s.src[i] = (uint8_t)(i + 1);
  s.pend_why = GAP_LOG_RESET; s.pend_from = -4; s.pend_to = 99;
  return s;
}

static void roundtrip(void) {
  PState s = sample(), o;
  uint8_t b[STATE_MAX_BYTES];
  uint32_t seq;
  size_t n = state_encode(&s, 77, b, sizeof b);
  CHECK(n > 0 && n <= STATE_MAX_BYTES);
  CHECK(state_decode(b, n, &o, &seq));
  CHECK_EQ(seq, 77);
  CHECK(memcmp(&s, &o, sizeof s) == 0);
  CHECK_EQ(state_encode(&s, 1, b, n - 1), 0);
}

static void every_corruption_is_rejected(void) {
  PState s = sample(), o;
  uint8_t b[STATE_MAX_BYTES], c[STATE_MAX_BYTES];
  uint32_t seq;
  size_t n = state_encode(&s, 5, b, sizeof b);
  for (size_t i = 0; i < n; i++)
    for (int bit = 0; bit < 8; bit++) {
      memcpy(c, b, n);
      c[i] ^= (uint8_t)(1 << bit);
      CHECK(!state_decode(c, n, &o, &seq));
    }
  for (size_t m = 0; m < n; m++) CHECK(!state_decode(b, m, &o, &seq));
  CHECK(!state_decode(b, n + 1, &o, &seq));
  memset(c, 0, n);
  CHECK(!state_decode(c, n, &o, &seq));
}

static void unknown_version_with_valid_crc_is_rejected(void) {
  PState s = sample(), o;
  uint8_t b[STATE_MAX_BYTES];
  uint32_t seq;
  size_t n = state_encode(&s, 5, b, sizeof b);
  b[4] = 2;
  uint32_t crc = crc32_of(b, n - 4);
  for (int i = 0; i < 4; i++) b[n - 4 + i] = (uint8_t)(crc >> (8 * i));
  CHECK(!state_decode(b, n, &o, &seq));
}

static void rejects_nonsense_fields(void) {
  PState s = sample(), o;
  uint8_t b[STATE_MAX_BYTES];
  uint32_t seq;
  s.ep = 0;
  size_t n = state_encode(&s, 1, b, sizeof b);
  CHECK(!state_decode(b, n, &o, &seq));
  s = sample(); s.cursor = -1;
  n = state_encode(&s, 1, b, sizeof b);
  CHECK(!state_decode(b, n, &o, &seq));
  s = sample(); s.pend_why = 200;
  n = state_encode(&s, 1, b, sizeof b);
  CHECK(!state_decode(b, n, &o, &seq));
}

static void put_slot(FakeFs *f, const char *path, const PState *s, uint32_t seq) {
  uint8_t b[STATE_MAX_BYTES];
  size_t n = state_encode(s, seq, b, sizeof b);
  ffs_set(f, path, b, n);
}

static void load_picks_higher_seq(void) {
  ffs_init(&F);
  Fs fs = ffs_iface();
  PState a = sample(), b = sample(), got;
  a.cursor = 100; b.cursor = 200;
  uint32_t seq; int next;
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_NONE);
  put_slot(&F, AW_PATH_STATE_A, &a, 4);
  put_slot(&F, AW_PATH_STATE_B, &b, 5);
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_OK);
  CHECK_EQ(got.cursor, 200); CHECK_EQ(seq, 5); CHECK_EQ(next, 0);
  put_slot(&F, AW_PATH_STATE_A, &a, 6);
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_OK);
  CHECK_EQ(got.cursor, 100); CHECK_EQ(seq, 6); CHECK_EQ(next, 1);
}

static void load_seq_tie_is_deterministic(void) {
  ffs_init(&F);
  Fs fs = ffs_iface();
  PState a = sample(), b = sample(), got;
  a.cursor = 1; b.cursor = 2;
  put_slot(&F, AW_PATH_STATE_A, &a, 9);
  put_slot(&F, AW_PATH_STATE_B, &b, 9);
  uint32_t seq; int next;
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_OK);
  CHECK_EQ(got.cursor, 1); CHECK_EQ(next, 1);
}

static void load_survives_one_bad_slot(void) {
  ffs_init(&F);
  Fs fs = ffs_iface();
  PState a = sample(), b = sample(), got;
  a.cursor = 100; b.cursor = 200;
  put_slot(&F, AW_PATH_STATE_A, &a, 4);
  put_slot(&F, AW_PATH_STATE_B, &b, 5);
  FFile *fb = ffs_find(&F, AW_PATH_STATE_B);
  fb->d[20] ^= 0x40;
  uint32_t seq; int next;
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_OK);
  CHECK_EQ(got.cursor, 100); CHECK_EQ(next, 1);
  fb->n = 0;                                   /* truncated by a crash */
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_OK);
  CHECK_EQ(got.cursor, 100);
}

static void load_reports_corrupt_and_ioerr(void) {
  ffs_init(&F);
  Fs fs = ffs_iface();
  PState a = sample(), got;
  uint32_t seq; int next;
  ffs_set(&F, AW_PATH_STATE_A, "garbage", 7);
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_CORRUPT);
  ffs_set(&F, AW_PATH_STATE_B, "", 0);
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_CORRUPT);
  put_slot(&F, AW_PATH_STATE_A, &a, 1);
  strcpy(F.read_err_path, "state.b"); F.read_err = EIO; F.read_err_n = -1;
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_IOERR);   /* never guess on a read error */
}

static void save_writes_the_requested_slot(void) {
  ffs_init(&F);
  Fs fs = ffs_iface();
  PState a = sample(), got;
  CHECK_EQ(state_save(&fs, &F, &a, 3, 1), 0);
  CHECK(ffs_find(&F, AW_PATH_STATE_B) != NULL);
  CHECK(ffs_find(&F, AW_PATH_STATE_A) == NULL);
  uint32_t seq; int next;
  CHECK_EQ(state_load(&fs, &F, &got, &seq, &next), ST_OK);
  CHECK_EQ(seq, 3); CHECK_EQ(next, 0);
}

int main(void) {
  RUN(roundtrip);
  RUN(every_corruption_is_rejected);
  RUN(unknown_version_with_valid_crc_is_rejected);
  RUN(rejects_nonsense_fields);
  RUN(load_picks_higher_seq);
  RUN(load_seq_tie_is_deterministic);
  RUN(load_survives_one_bad_slot);
  RUN(load_reports_corrupt_and_ioerr);
  RUN(save_writes_the_requested_slot);
  return t_done("state");
}
