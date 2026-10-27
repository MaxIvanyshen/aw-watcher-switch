#include "sync_priv.h"
#include "sha256.h"
#include "util.h"
#include <errno.h>
#include <string.h>

typedef struct {
  uint8_t head[64], tail[64];
  uint32_t head_len, tail_len;
} Roll;

uint8_t *spool_body(SyncEngine *e) { return e->req + SYNC_HDR_MAX; }

static int hexv(uint8_t c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* content excludes the newline */
int spool_line_valid(const uint8_t *p, size_t n) {
  static const char pre[] = "{\"v\":1,";
  uint32_t want = 0;
  size_t i;
  if (n < 7 + 16 || n > SYNC_LINE_MAX - 1 || memcmp(p, pre, 7) != 0) return 0;
  for (i = 0; i < n; i++) if (p[i] < 0x20 || p[i] > 0x7e) return 0;
  if (memcmp(p + n - 16, ",\"c\":\"", 6) != 0 || p[n-2] != '"' || p[n-1] != '}') return 0;
  for (i = 0; i < 8; i++) {
    int h = hexv(p[n - 10 + i]);
    if (h < 0) return 0;
    want = want << 4 | (uint32_t)h;
  }
  return crc32_of(p, n - 16) == want;
}

static void feed(Roll *r, uint64_t pos, const uint8_t *p, size_t n) {
  if (r->head_len < 64 && pos == r->head_len) {
    size_t k = 64 - r->head_len < n ? 64 - r->head_len : n;
    memcpy(r->head + r->head_len, p, k);
    r->head_len += (uint32_t)k;
  }
  if (n >= 64) { memcpy(r->tail, p + n - 64, 64); r->tail_len = 64; return; }
  if (r->tail_len + n > 64) {
    size_t keep = 64 - n;
    memmove(r->tail, r->tail + r->tail_len - keep, keep);
    r->tail_len = (uint32_t)keep;
  }
  memcpy(r->tail + r->tail_len, p, n);
  r->tail_len += (uint32_t)n;
}

static int win_fill(SyncEngine *e, uint64_t off) {
  size_t got = 0;
  int rc;
  e->win_valid = 0;
  rc = e->plat.read_at(e->plat.ctx, e->cfg.journal, off, e->win, sizeof e->win, &got);
  if (rc < 0) return rc;
  if (rc > 0 || got > sizeof e->win) return -EIO;
  e->win_off = off; e->win_len = (uint32_t)got; e->win_full = got == sizeof e->win; e->win_valid = 1;
  return 0;
}

/* Skips the rest of a line longer than SYNC_LINE_MAX. *term is 1 when its newline was found. */
static int skip_long(SyncEngine *e, Roll *r, uint64_t pos, uint64_t *total, int *term) {
  uint64_t cur = pos;
  *total = 0; *term = 0;
  for (;;) {
    const uint8_t *nl;
    int rc = win_fill(e, cur);
    if (rc) return rc;
    if (e->win_len == 0) return 0;
    nl = memchr(e->win, '\n', e->win_len);
    if (nl) { size_t c = (size_t)(nl - e->win) + 1; feed(r, cur, e->win, c); *total += c; *term = 1; return 0; }
    feed(r, cur, e->win, e->win_len);
    *total += e->win_len; cur += e->win_len;
    if (!e->win_full) return 0;
  }
}

int spool_scan(SyncEngine *e, Batch *b) {
  Roll r;
  uint8_t *body = spool_body(e);
  uint64_t pos = e->ck.offset;
  uint8_t etail[64];
  uint32_t etail_len = 0;
  memset(b, 0, sizeof *b);
  memset(&r, 0, sizeof r);
  memcpy(r.head, e->vhead, e->ck.anchor_len); r.head_len = e->ck.anchor_len;
  memcpy(r.tail, e->vtail, e->ck.tail_len); r.tail_len = e->ck.tail_len;
  b->start = b->end = pos;
  e->win_valid = 0;
  e->held_bytes = 0;
  for (;;) {
    const uint8_t *p, *nl;
    uint64_t avail;
    size_t len, content;
    int term;
    if (b->count >= SYNC_BATCH_MAX_RECORDS) { b->more = 1; break; }
    if (!e->win_valid || pos < e->win_off || pos > e->win_off + e->win_len) { int rc = win_fill(e, pos); if (rc) return rc; }
    avail = e->win_off + e->win_len - pos;
    if (avail < SYNC_LINE_MAX + 1 && e->win_full) { int rc = win_fill(e, pos); if (rc) return rc; avail = e->win_len; }
    if (avail == 0) break;
    p = e->win + (pos - e->win_off);
    nl = memchr(p, '\n', avail < SYNC_LINE_MAX + 1 ? avail : SYNC_LINE_MAX + 1);
    if (!nl && avail > SYNC_LINE_MAX) {
      uint64_t total; int rc;
      rc = skip_long(e, &r, pos, &total, &term);
      if (rc) return rc;
      if (!term) { b->held = (uint32_t)(total > 0xffffffffu ? 0xffffffffu : total); break; }
      b->sk_lines++; b->sk_bytes += (uint32_t)(total > 0xffffffffu ? 0xffffffffu : total);
      pos += total;
      continue;
    }
    term = nl != NULL;
    len = term ? (size_t)(nl - p) + 1 : (size_t)avail;
    content = term ? len - 1 : len;
    if (term && len == 1) { feed(&r, pos, p, 1); pos += 1; continue; }
    if (len <= SYNC_LINE_MAX && spool_line_valid(p, content)) {
      if (b->body_len + content + 1 > SYNC_BATCH_MAX_BYTES) { b->more = 1; break; }
      memcpy(body + b->body_len, p, content);
      body[b->body_len + content] = '\n';
      b->body_len += (uint32_t)content + 1;
      b->count++;
      feed(&r, pos, p, len);
      pos += len;
      b->end = pos;
      memcpy(etail, r.tail, r.tail_len); etail_len = r.tail_len;
      b->sk_lines_in = b->sk_lines; b->sk_bytes_in = b->sk_bytes;
      continue;
    }
    if (!term) { b->held = (uint32_t)len; break; }
    feed(&r, pos, p, len);
    b->sk_lines++; b->sk_bytes += (uint32_t)len;
    pos += len;
  }
  e->held_bytes = b->held;
  if (b->count) {
    b->anchor_len = b->end < 64 ? (uint32_t)b->end : 64;
    b->tail_len = etail_len;
    memcpy(b->ahead, r.head, b->anchor_len);
    memcpy(b->atail, etail, etail_len);
    sha256_of(r.head, b->anchor_len, b->anchor_sha);
    sha256_of(etail, etail_len, b->tail_sha);
    sha256_of(body, b->body_len, b->body_sha);
  }
  return 0;
}

static int read_exact(SyncEngine *e, uint64_t off, uint8_t *buf, uint32_t n) {
  size_t got = 0;
  int rc;
  if (n == 0) return 0;
  rc = e->plat.read_at(e->plat.ctx, e->cfg.journal, off, buf, n, &got);
  if (rc < 0) return rc;
  if (rc > 0 || got > n) return -EIO;
  return got == n ? 0 : 1;
}

int spool_verify(SyncEngine *e, uint64_t size, uint64_t end, uint32_t alen, const uint8_t *ash, uint32_t tlen,
                 const uint8_t *tsh, uint8_t *vh, uint8_t *vt) {
  uint8_t a[64], t[64], d[32];
  int rc;
  if (size < end || alen > 64 || tlen > 64 || tlen > end || alen > end) return 1;
  rc = read_exact(e, 0, a, alen);
  if (rc) return rc;
  rc = read_exact(e, end - tlen, t, tlen);
  if (rc) return rc;
  sha256_of(a, alen, d);
  if (memcmp(d, ash, 32) != 0) return 1;
  sha256_of(t, tlen, d);
  if (memcmp(d, tsh, 32) != 0) return 1;
  if (vh) memcpy(vh, a, alen);
  if (vt) memcpy(vt, t, tlen);
  return 0;
}
