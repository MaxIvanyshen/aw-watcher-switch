#include "state.h"

#include <errno.h>
#include <string.h>

#include "paths.h"
#include "util.h"

#define HDR 12
#define PAYLOAD 88

static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }

size_t state_encode(const PState *s, uint32_t seq, uint8_t *buf, size_t cap) {
  size_t total = HDR + PAYLOAD + 4;
  if (cap < total) return 0;
  memset(buf, 0, total);
  p32(buf, STATE_MAGIC);
  buf[4] = STATE_VERSION; buf[5] = 0;
  buf[6] = PAYLOAD; buf[7] = 0;
  p32(buf + 8, seq);
  uint8_t *p = buf + HDR;
  p32(p, s->ep); p32(p + 4, (uint32_t)s->cursor); p32(p + 8, s->anchor);
  p[12] = (uint8_t)(s->anchor_ok | s->tr.open << 1 | s->tr.prev_valid << 2 | s->src_ok << 3);
  p[13] = s->tr.pol; p[14] = s->pend_why;
  p64(p + 16, s->tr.pid); p64(p + 24, s->tr.su); p64(p + 32, s->tr.sn); p64(p + 40, s->tr.ss);
  p64(p + 48, s->tr.alive); p64(p + 56, s->tr.prev_steady);
  memcpy(p + 64, s->src, 16);
  p32(p + 80, (uint32_t)s->pend_from); p32(p + 84, (uint32_t)s->pend_to);
  p32(buf + HDR + PAYLOAD, crc32_of(buf, HDR + PAYLOAD));
  return total;
}

int state_decode(const uint8_t *buf, size_t n, PState *s, uint32_t *seq) {
  if (n != HDR + PAYLOAD + 4) return 0;
  if (g32(buf) != STATE_MAGIC || buf[4] != STATE_VERSION || buf[5] != 0 || buf[6] != PAYLOAD || buf[7] != 0) return 0;
  if (g32(buf + HDR + PAYLOAD) != crc32_of(buf, HDR + PAYLOAD)) return 0;
  const uint8_t *p = buf + HDR;
  PState o;
  memset(&o, 0, sizeof o);
  o.ep = g32(p); o.cursor = (int32_t)g32(p + 4); o.anchor = g32(p + 8);
  uint8_t f = p[12];
  if (f & ~0x0f) return 0;
  o.anchor_ok = f & 1; o.tr.open = (f >> 1) & 1; o.tr.prev_valid = (f >> 2) & 1; o.src_ok = (f >> 3) & 1;
  o.tr.pol = p[13]; o.pend_why = p[14];
  o.tr.pid = g64(p + 16); o.tr.su = g64(p + 24); o.tr.sn = g64(p + 32); o.tr.ss = g64(p + 40);
  o.tr.alive = g64(p + 48); o.tr.prev_steady = g64(p + 56);
  memcpy(o.src, p + 64, 16);
  o.pend_from = (int32_t)g32(p + 80); o.pend_to = (int32_t)g32(p + 84);
  if (o.ep == 0 || o.cursor < 0 || o.pend_why >= GAP_COUNT || p[15] != 0) return 0;
  *s = o;
  *seq = g32(buf + 8);
  return 1;
}

static const char *const slot_path[2] = {AW_PATH_STATE_A, AW_PATH_STATE_B};

StateLoad state_load(const Fs *fs, void *fu, PState *s, uint32_t *seq, int *next_slot) {
  PState got[2];
  uint32_t sq[2] = {0, 0};
  int valid[2] = {0, 0}, exists[2] = {0, 0};
  for (int i = 0; i < 2; i++) {
    uint8_t buf[STATE_MAX_BYTES];
    size_t n = 0;
    int rc = fs->read_file(fu, slot_path[i], buf, sizeof buf, &n);
    if (rc == ENOENT) continue;
    if (rc != 0) return ST_IOERR;
    exists[i] = 1;
    valid[i] = n <= sizeof buf && state_decode(buf, n, &got[i], &sq[i]);
  }
  if (!valid[0] && !valid[1]) return (exists[0] || exists[1]) ? ST_CORRUPT : ST_NONE;
  int win = !valid[0] ? 1 : !valid[1] ? 0 : (sq[1] > sq[0] ? 1 : 0);
  *s = got[win];
  *seq = sq[win];
  *next_slot = 1 - win;
  return ST_OK;
}

int state_save(const Fs *fs, void *fu, const PState *s, uint32_t seq, int slot) {
  uint8_t buf[STATE_MAX_BYTES];
  size_t n = state_encode(s, seq, buf, sizeof buf);
  if (n == 0) return EINVAL;
  return fs->write_file(fu, slot_path[slot], buf, n);
}
