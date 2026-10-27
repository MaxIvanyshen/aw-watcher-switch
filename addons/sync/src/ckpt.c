#include "sync_priv.h"
#include "util.h"
#include <errno.h>
#include <string.h>

static void p32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8*i)); }
static void p64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8*i)); }
static uint32_t g32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t g64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

void ckpt_encode(const SyncCkpt *c, uint8_t out[128]) {
  memset(out, 0, 128);
  memcpy(out, "AWY1", 4);
  p32(out + 4, c->seq); p64(out + 8, c->offset); p64(out + 16, c->acked);
  p32(out + 24, c->anchor_len); p32(out + 28, c->tail_len);
  memcpy(out + 32, c->anchor_sha, 32); memcpy(out + 64, c->tail_sha, 32);
  p32(out + 124, crc32_of(out, 124));
}

int ckpt_decode(const uint8_t in[128], SyncCkpt *c) {
  uint64_t want;
  if (memcmp(in, "AWY1", 4) != 0 || g32(in + 124) != crc32_of(in, 124)) return -EINVAL;
  c->seq = g32(in + 4); c->offset = g64(in + 8); c->acked = g64(in + 16);
  c->anchor_len = g32(in + 24); c->tail_len = g32(in + 28);
  want = c->offset < 64 ? c->offset : 64;
  if (c->anchor_len != want || c->tail_len != want) return -EINVAL;
  memcpy(c->anchor_sha, in + 32, 32); memcpy(c->tail_sha, in + 64, 32);
  return 0;
}
