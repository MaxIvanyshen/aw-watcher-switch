#include "fakes.h"
#include "sync_priv.h"
#include "sha256.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FkFile *find(Fake *f, const char *path, int create) {
  FkFile *free_slot = NULL;
  for (int i = 0; i < FK_FILES; i++) {
    if (f->files[i].used && !strcmp(f->files[i].path, path)) return &f->files[i];
    if (!f->files[i].used && !free_slot) free_slot = &f->files[i];
  }
  if (!create || !free_slot) return NULL;
  memset(free_slot, 0, sizeof *free_slot);
  snprintf(free_slot->path, sizeof free_slot->path, "%s", path);
  free_slot->used = 1;
  return free_slot;
}

static void fgrow(FkFile *x, size_t n) {
  if (n <= x->n) return;
  x->d = realloc(x->d, n);
  memset(x->d + x->n, 0, n - x->n);
  x->n = n;
}

void fk_init(Fake *f, const char *token, const char *device) {
  memset(f, 0, sizeof *f);
  f->recs = calloc(FK_RECS, sizeof(FkRec));
  f->token = token; f->device = device;
  f->now = 1000;
}

void fk_free(Fake *f) {
  for (int i = 0; i < FK_FILES; i++) free(f->files[i].d);
  free(f->recs);
}

void fk_put(Fake *f, const char *path, const void *d, size_t n) {
  FkFile *x = find(f, path, 1);
  free(x->d); x->d = NULL; x->n = 0;
  if (n) { fgrow(x, n); memcpy(x->d, d, n); } else x->d = malloc(1);
}

void fk_append(Fake *f, const char *path, const void *d, size_t n) {
  FkFile *x = find(f, path, 1);
  size_t o = x->n;
  fgrow(x, o + n);
  memcpy(x->d + o, d, n);
}

const uint8_t *fk_get(Fake *f, const char *path, size_t *n) {
  FkFile *x = find(f, path, 0);
  if (!x) return NULL;
  *n = x->n;
  return x->d;
}

void fk_del(Fake *f, const char *path) {
  FkFile *x = find(f, path, 0);
  if (x) { free(x->d); memset(x, 0, sizeof *x); }
}

size_t fk_nrecs(const Fake *f) { return f->nrecs; }
int fk_has(const Fake *f, long ep, long ev, long sub) {
  for (size_t i = 0; i < f->nrecs; i++) if (f->recs[i].ep == ep && f->recs[i].ev == ev && f->recs[i].sub == sub) return 1;
  return 0;
}

int fk_writes_clean(const Fake *f, const char *sd, const char *journal) {
  for (size_t i = 0; i < f->nw; i++) {
    const char *p = f->wlog[i];
    if (!sync_path_under(p, sd) || sync_path_under(p, "/config/aw-watcher-switch") || !strcmp(p, journal)) return 0;
  }
  return 1;
}

static int failable(Fake *f) {
  f->calls++;
  return f->fail_at && f->calls == f->fail_at ? -f->fail_err : 0;
}

static int f_size(void *c, const char *path, uint64_t *size) {
  Fake *f = c; FkFile *x; int rc = failable(f);
  if (rc) return rc;
  x = find(f, path, 0);
  if (!x) return -ENOENT;
  *size = x->n;
  return 0;
}

static int f_read(void *c, const char *path, uint64_t off, void *buf, size_t cap, size_t *got) {
  Fake *f = c; FkFile *x; int rc = failable(f);
  if (rc) return rc;
  x = find(f, path, 0);
  if (!x) return -ENOENT;
  if (!strcmp(path, SYNC_PATH_JOURNAL) || strstr(path, "journal")) {
    f->journal_reads++;
    if (f->read_fail_after && f->journal_reads > f->read_fail_after) return -f->journal_read_err;
  }
  *got = off >= x->n ? 0 : (x->n - off < cap ? x->n - off : cap);
  if (*got) memcpy(buf, x->d + off, *got);
  return 0;
}

static int f_write(void *c, const char *path, uint64_t off, const void *buf, size_t n) {
  Fake *f = c; FkFile *x; int rc = failable(f);
  if (f->nw < 256) snprintf(f->wlog[f->nw++], 128, "%s", path);
  if (rc) return rc;
  if (f->fail_write_prefix && strstr(path, "/sync.")) return -EIO;
  x = find(f, path, 1);
  if (!x) return -ENOSPC;
  fgrow(x, off + n);
  memcpy(x->d + off, buf, n);
  return 0;
}

static uint64_t f_now(void *c) { return ((Fake *)c)->now; }
static int f_net_open(void *c) { Fake *f = c; if (f->net_fail) return -f->net_fail; f->net_open_n++; return 0; }
static void f_net_close(void *c) { ((Fake *)c)->net_close_n++; }

static const char *hdr(const char *req, const char *name, char *out, size_t cap) {
  size_t k = strlen(name);
  const char *p = req;
  while ((p = strstr(p, name)) && !(p == req || p[-1] == '\n')) p++;
  if (!p) return NULL;
  p += k;
  size_t i = 0;
  while (*p != '\r' && i + 1 < cap) out[i++] = *p++;
  out[i] = 0;
  return out;
}

static long kfield(const char *l, const char *name) {
  const char *p = strstr(l, name);
  return p ? strtol(p + strlen(name), NULL, 10) : -1;
}

static size_t respond(char *out, size_t cap, int status, const char *body, int cl_adj, const char *extra) {
  return (size_t)snprintf(out, cap, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n%sContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                          status, extra, strlen(body) + (size_t)cl_adj, body);
}

/* Section 4 rules: auth, device, hash, line validation, record and batch dedup, one atomic transaction. */
static size_t serve(Fake *f, const uint8_t *req, size_t n, char *out, size_t cap) {
  char tmp[256], id[64], sha[80], dev[64];
  const char *hend = NULL;
  char *rq = malloc(n + 1);
  size_t blen, nl = 0, pos = 0, ret;
  const uint8_t *body;
  long count;
  uint8_t d[32];
  char hex[65];
  struct { long ep, ev, sub; const uint8_t *l; size_t len; } ln[300];
  unsigned nnew = 0, ndup = 0;
  memcpy(rq, req, n); rq[n] = 0;
  hend = strstr(rq, "\r\n\r\n");
  if (!hend) { ret = respond(out, cap, 400, "{\"v\":1,\"error\":\"bad\"}", 0, ""); goto done; }
  body = req + (hend + 4 - rq); blen = n - (size_t)(hend + 4 - rq);
  snprintf(tmp, sizeof tmp, "Bearer %s", f->token);
  if (!hdr(rq, "Authorization: ", id, sizeof id) || strcmp(id, tmp)) { ret = respond(out, cap, 401, "{\"v\":1,\"error\":\"auth\"}", 0, ""); goto done; }
  if (!hdr(rq, "X-Device: ", dev, sizeof dev) || strcmp(dev, f->device)) { ret = respond(out, cap, 403, "{\"v\":1,\"error\":\"device\"}", 0, ""); goto done; }
  if (!hdr(rq, "X-Batch-Id: ", id, sizeof id) || !hdr(rq, "X-Batch-Sha256: ", sha, sizeof sha) || !hdr(rq, "X-Batch-Count: ", tmp, sizeof tmp)) { ret = respond(out, cap, 400, "{\"v\":1,\"error\":\"bad\"}", 0, ""); goto done; }
  count = atol(tmp);
  sha256_of(body, blen, d); sha256_hex(d, hex);
  if (strcmp(hex, sha) != 0 || blen > 65536) { ret = respond(out, cap, 400, "{\"v\":1,\"error\":\"hash\"}", 0, ""); goto done; }
  if (blen == 0 || body[blen - 1] != '\n') { ret = respond(out, cap, 400, "{\"v\":1,\"error\":\"line\"}", 0, ""); goto done; }
  while (pos < blen) {
    const uint8_t *e = memchr(body + pos, '\n', blen - pos);
    size_t len = (size_t)(e - (body + pos));
    char tl[SYNC_LINE_MAX + 1];
    if (nl >= 300 || len + 1 > SYNC_LINE_MAX || !spool_line_valid(body + pos, len)) { ret = respond(out, cap, 400, "{\"v\":1,\"error\":\"line\"}", 0, ""); goto done; }
    memcpy(tl, body + pos, len); tl[len] = 0;
    ln[nl].ep = kfield(tl, "\"ep\":"); ln[nl].ev = kfield(tl, "\"ev\":"); ln[nl].sub = kfield(tl, "\"sub\":");
    ln[nl].l = body + pos; ln[nl].len = len; nl++;
    pos += len + 1;
  }
  if ((long)nl != count) { ret = respond(out, cap, 400, "{\"v\":1,\"error\":\"count\"}", 0, ""); goto done; }
  for (size_t i = 0; i < f->nbatches; i++) if (!strcmp(f->batches[i].id, id)) {
    if (strcmp(f->batches[i].sha, sha) != 0) { ret = respond(out, cap, 409, "{\"v\":1,\"error\":\"conflict\",\"key\":\"batch\"}", 0, ""); goto done; }
    ret = respond(out, cap, 200, f->batches[i].ack, 0, "");
    memcpy(f->last_body_sha, d, 32); snprintf(f->last_id, sizeof f->last_id, "%s", id);
    goto done;
  }
  for (size_t i = 0; i < nl; i++) {
    int found = 0;
    for (size_t j = 0; j < f->nrecs; j++) if (f->recs[j].ep == ln[i].ep && f->recs[j].ev == ln[i].ev && f->recs[j].sub == ln[i].sub) {
      found = 1;
      if (strlen(f->recs[j].line) != ln[i].len || memcmp(f->recs[j].line, ln[i].l, ln[i].len)) {
        char b[96]; snprintf(b, sizeof b, "{\"v\":1,\"error\":\"conflict\",\"key\":\"%ld/%ld/%ld\"}", ln[i].ep, ln[i].ev, ln[i].sub);
        ret = respond(out, cap, 409, b, 0, ""); goto done;
      }
    }
    for (size_t k = 0; k < i && !found; k++) if (ln[k].ep == ln[i].ep && ln[k].ev == ln[i].ev && ln[k].sub == ln[i].sub) {
      found = 1;
      if (ln[k].len != ln[i].len || memcmp(ln[k].l, ln[i].l, ln[i].len)) { ret = respond(out, cap, 409, "{\"v\":1,\"error\":\"conflict\",\"key\":\"dup\"}", 0, ""); goto done; }
    }
    (void)found;
  }
  for (size_t i = 0; i < nl; i++) {
    int found = 0;
    for (size_t j = 0; j < f->nrecs; j++) if (f->recs[j].ep == ln[i].ep && f->recs[j].ev == ln[i].ev && f->recs[j].sub == ln[i].sub) found = 1;
    if (found) { ndup++; continue; }
    if (f->nrecs >= FK_RECS) { ret = respond(out, cap, 500, "{}", 0, ""); goto done; }
    f->recs[f->nrecs].ep = ln[i].ep; f->recs[f->nrecs].ev = ln[i].ev; f->recs[f->nrecs].sub = ln[i].sub;
    memcpy(f->recs[f->nrecs].line, ln[i].l, ln[i].len); f->recs[f->nrecs].line[ln[i].len] = 0;
    f->nrecs++; nnew++;
  }
  if (f->nbatches < FK_BATCHES) {
    FkBatch *b = &f->batches[f->nbatches++];
    snprintf(b->id, sizeof b->id, "%s", id); snprintf(b->sha, sizeof b->sha, "%s", sha);
    snprintf(b->ack, sizeof b->ack, "{\"v\":1,\"id\":\"%s\",\"sha256\":\"%s\",\"count\":%ld,\"new\":%u,\"dup\":%u}", id, sha, count, nnew, ndup);
    ret = respond(out, cap, 200, b->ack, 0, "");
  } else ret = respond(out, cap, 500, "{}", 0, "");
  f->last_new = nnew; f->last_dup = ndup;
  memcpy(f->last_body_sha, d, 32); snprintf(f->last_id, sizeof f->last_id, "%s", id);
done:
  free(rq);
  return ret;
}

static int f_exchange(void *c, const uint8_t ip[4], uint16_t port, const void *req, size_t req_len, void *resp,
                      size_t resp_cap, size_t *resp_len, uint64_t deadline) {
  Fake *f = c;
  char out[2048];
  size_t n;
  int rc = failable(f), idx = f->exch_n++;
  FkMode m = f->mode;
  (void)ip; (void)port;
  *resp_len = 0;
  if (rc) return rc;
  if (deadline <= f->now) return -ETIMEDOUT;
  if (f->nsent < FK_BATCHES) {
    char id[64];
    char *rq = malloc(req_len + 1);
    memcpy(rq, req, req_len); rq[req_len] = 0;
    if (hdr(rq, "X-Batch-Id: ", id, sizeof id)) snprintf(f->sent_ids[f->nsent++], 48, "%s", id);
    free(rq);
  }
  if (f->hook) f->hook(f, 0, idx);
  if (f->mode_left == 0) m = M_OK; else if (f->mode_left > 0) f->mode_left--;
  n = serve(f, req, req_len, out, sizeof out);
  if (f->hook) f->hook(f, 1, idx);
  f->now += f->exch_advance;
  if (m == M_DROP_RESP) return -ETIMEDOUT;
  if (m != M_OK && strstr(out, "HTTP/1.1 200")) {
    char *b = strstr(out, "\r\n\r\n") + 4;
    char ack[300];
    snprintf(ack, sizeof ack, "%s", b);
    switch (m) {
      case M_WRONG_ID: { char *p = strstr(ack, "\"id\":\"") + 6; *p = *p == 'f' ? '0' : 'f'; n = respond(out, sizeof out, 200, ack, 0, ""); break; }
      case M_WRONG_SHA: { char *p = strstr(ack, "\"sha256\":\"") + 10; *p = *p == 'f' ? '0' : 'f'; n = respond(out, sizeof out, 200, ack, 0, ""); break; }
      case M_WRONG_COUNT: { char *p = strstr(ack, "\"count\":") + 8; *p = *p == '9' ? '8' : (char)(*p + 1); n = respond(out, sizeof out, 200, ack, 0, ""); break; }
      case M_BAD_SUM: { char *p = strstr(ack, "\"dup\":") + 6; *p = *p == '9' ? '8' : (char)(*p + 1); n = respond(out, sizeof out, 200, ack, 0, ""); break; }
      case M_S500: n = respond(out, sizeof out, 500, "{}", 0, ""); break;
      case M_TRUNC: n = respond(out, sizeof out, 200, ack, 5, ""); n -= 5; break;
      case M_BIG_CL: { char big[700]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0; n = respond(out, sizeof out, 200, big, 0, ""); break; }
      case M_EXTRA: strcat(ack, "x"); n = respond(out, sizeof out, 200, ack, -1, ""); break;
      case M_OLD_ACK: if (f->nbatches > 1) n = respond(out, sizeof out, 200, f->batches[f->nbatches - 2].ack, 0, ""); else n = respond(out, sizeof out, 200, "{\"v\":1}", 0, ""); break;
      case M_NO_CL: n = (size_t)snprintf(out, sizeof out, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n%s", ack); break;
      case M_CHUNKED: n = respond(out, sizeof out, 200, ack, 0, "Transfer-Encoding: chunked\r\n"); break;
      case M_S400: n = respond(out, sizeof out, 400, "{\"v\":1,\"error\":\"x\"}", 0, ""); break;
      case M_S401: n = respond(out, sizeof out, 401, "{}", 0, ""); break;
      case M_S403: n = respond(out, sizeof out, 403, "{}", 0, ""); break;
      case M_S409: n = respond(out, sizeof out, 409, "{\"v\":1,\"error\":\"conflict\",\"key\":\"1/2/1\"}", 0, ""); break;
      case M_S413: n = respond(out, sizeof out, 413, "{}", 0, ""); break;
      case M_GARBAGE: n = (size_t)snprintf(out, sizeof out, "garbage"); break;
      default: break;
    }
  }
  if (n > resp_cap) return -EMSGSIZE;
  memcpy(resp, out, n);
  *resp_len = n;
  return 0;
}

void fk_plat(Fake *f, SyncPlat *p) {
  p->ctx = f; p->file_size = f_size; p->read_at = f_read; p->write_at = f_write; p->now_ms = f_now;
  p->net_open = f_net_open; p->net_close = f_net_close; p->exchange = f_exchange;
}
