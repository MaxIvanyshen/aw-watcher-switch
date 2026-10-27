#include "sync_priv.h"
#include "util.h"
#include <string.h>

size_t http_build(SyncEngine *e, const Batch *b, const char *id, const char *shahex, uint8_t **req) {
  char h[SYNC_HDR_MAX];
  SBuf s;
  size_t n;
  sb_init(&s, h, sizeof h);
  sb_str(&s, "POST /v1/batch HTTP/1.1\r\nHost: ");
  for (int i = 0; i < 4; i++) { sb_u64(&s, e->cfg.ip[i]); if (i < 3) sb_ch(&s, '.'); }
  sb_ch(&s, ':'); sb_u64(&s, e->cfg.port);
  sb_str(&s, "\r\nAuthorization: Bearer "); sb_str(&s, e->cfg.token);
  sb_str(&s, "\r\nX-Device: "); sb_str(&s, e->cfg.device);
  sb_str(&s, "\r\nX-Batch-Id: "); sb_str(&s, id);
  sb_str(&s, "\r\nX-Batch-Count: "); sb_u64(&s, b->count);
  sb_str(&s, "\r\nX-Batch-Sha256: "); sb_str(&s, shahex);
  sb_str(&s, "\r\nContent-Type: application/x-ndjson\r\nContent-Length: "); sb_u64(&s, b->body_len);
  sb_str(&s, "\r\nConnection: close\r\n\r\n");
  if (s.ovf) return 0;
  n = s.len;
  memcpy(e->req + SYNC_HDR_MAX - n, h, n);
  *req = e->req + SYNC_HDR_MAX - n;
  return n + b->body_len;
}

static int ieq_prefix(const char *p, size_t n, const char *name) {
  size_t k = strlen(name);
  if (n < k) return 0;
  for (size_t i = 0; i < k; i++) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    if (c != name[i]) return 0;
  }
  return 1;
}

static int dec(const char **p, const char *end, unsigned max, unsigned *v) {
  const char *s = *p;
  unsigned x = 0, d = 0;
  while (s < end && *s >= '0' && *s <= '9') { x = x * 10 + (unsigned)(*s - '0'); if (x > max) return -1; s++; d++; }
  if (d == 0 || (d > 1 && **p == '0')) return -1;
  *p = s; *v = x;
  return 0;
}

static void grab_key(const char *body, size_t n, char out[25]) {
  static const char pat[] = "\"key\":\"";
  out[0] = 0;
  for (size_t i = 0; i + sizeof pat - 1 < n; i++) {
    if (memcmp(body + i, pat, sizeof pat - 1) == 0) {
      size_t j = i + sizeof pat - 1, k = 0;
      while (j < n && k < 24 && ((body[j] >= '0' && body[j] <= '9') || body[j] == '/' || body[j] == '-')) out[k++] = body[j++];
      out[k] = 0;
      return;
    }
  }
  for (size_t i = 0; i + 7 <= n; i++) if (memcmp(body + i, "\"batch\"", 7) == 0) { strcpy(out, "batch"); return; }
}

int http_check(const uint8_t *r8, size_t n, const char *id, const char *shahex, uint32_t count, int *http, char conflict[25]) {
  const char *r = (const char *)r8, *hend = NULL, *body, *line, *end;
  size_t blen, i;
  int status, have_cl = 0;
  unsigned cl = 0;
  *http = 0;
  if (n < 12 || (memcmp(r, "HTTP/1.1 ", 9) != 0 && memcmp(r, "HTTP/1.0 ", 9) != 0)) return 1;
  for (i = 9; i < 12; i++) if (r[i] < '0' || r[i] > '9') return 1;
  if (n > 12 && r[12] != ' ' && r[12] != '\r') return 1;
  status = (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
  *http = status;
  for (i = 0; i + 3 < n; i++) if (memcmp(r + i, "\r\n\r\n", 4) == 0) { hend = r + i; break; }
  if (!hend) return 1;
  body = hend + 4;
  blen = n - (size_t)(body - r);
  for (line = (const char *)memchr(r, '\r', (size_t)(hend + 2 - r)) + 2; line < hend + 2; ) {
    const char *le = line;
    while (le < hend + 1 && !(le[0] == '\r' && le[1] == '\n')) le++;
    if (ieq_prefix(line, (size_t)(le - line), "content-length:")) {
      const char *v = line + 15;
      if (have_cl) return 1;
      while (v < le && *v == ' ') v++;
      if (dec(&v, le, 512, &cl) || v != le) return 1;
      have_cl = 1;
    } else if (ieq_prefix(line, (size_t)(le - line), "transfer-encoding:")) {
      return 1;
    }
    line = le + 2;
  }
  if (status == 400 || status == 401 || status == 403 || status == 409 || status == 413) {
    conflict[0] = 0;
    if (status == 409) grab_key(body, blen < 512 ? blen : 512, conflict);
    return 2;
  }
  if (status != 200 || !have_cl) return 1;
  if (blen != cl && !(blen == cl + 1 && body[cl] == '\n')) return 1;
  {
    char exp[160];
    SBuf s;
    unsigned k, j;
    const char *q;
    sb_init(&s, exp, sizeof exp);
    sb_str(&s, "{\"v\":1,\"id\":\""); sb_str(&s, id); sb_str(&s, "\",\"sha256\":\""); sb_str(&s, shahex);
    sb_str(&s, "\",\"count\":"); sb_u64(&s, count); sb_str(&s, ",");
    if (s.ovf || cl < s.len || memcmp(body, exp, s.len) != 0) return 1;
    q = body + s.len; end = body + cl;
    if ((size_t)(end - q) < 8 || memcmp(q, "\"new\":", 6) != 0) return 1;
    q += 6;
    if (dec(&q, end, 1000, &k) || end - q < 7 || memcmp(q, ",\"dup\":", 7) != 0) return 1;
    q += 7;
    if (dec(&q, end, 1000, &j) || end - q != 1 || *q != '}') return 1;
    if (k + j != count) return 1;
  }
  return 0;
}
