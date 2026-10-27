#include "sync_priv.h"
#include "sha256.h"
#include "util.h"
#include <errno.h>
#include <string.h>

#define STATUS_LEN 320

void sync_init(SyncEngine *e, const SyncPlat *p, const SyncCfg *cfg) {
  memset(e, 0, sizeof *e);
  e->plat = *p;
  e->cfg = *cfg;
  strcpy(e->stage, "init");
  strcpy(e->result, "idle");
}

static int st_path(const SyncEngine *e, const char *name, char path[112]) {
  size_t d = strlen(e->cfg.state_dir);
  if (d + 1 + strlen(name) >= 112) return -EINVAL;
  memcpy(path, e->cfg.state_dir, d);
  path[d] = '/';
  strcpy(path + d + 1, name);
  return 0;
}

/* The only write path. Refuses anything outside state_dir, the journal itself and the watcher directory. */
static int st_write(SyncEngine *e, const char *name, const void *buf, size_t n) {
  char path[112];
  if (sync_cfg_paths_ok(&e->cfg) || st_path(e, name, path) || !sync_path_under(path, e->cfg.state_dir) ||
      sync_path_under(path, "/config/aw-watcher-switch") || !strcmp(path, e->cfg.journal)) return -EPERM;
  return e->plat.write_at(e->plat.ctx, path, 0, buf, n);
}

/* 0 slot decoded, 1 absent or invalid, <0 I/O error */
static int slot_read(SyncEngine *e, const char *name, SyncCkpt *c) {
  char path[112];
  uint8_t buf[128];
  uint64_t size;
  size_t got = 0;
  int rc = st_path(e, name, path);
  if (rc) return rc;
  rc = e->plat.file_size(e->plat.ctx, path, &size);
  if (rc == -ENOENT) return 1;
  if (rc < 0) return rc;
  if (size < sizeof buf) return 1;
  rc = e->plat.read_at(e->plat.ctx, path, 0, buf, sizeof buf, &got);
  if (rc < 0) return rc;
  if (got != sizeof buf) return 1;
  return ckpt_decode(buf, c) ? 1 : 0;
}

static int ckpt_load(SyncEngine *e) {
  SyncCkpt a, b, *best = NULL;
  int ra = slot_read(e, SYNC_NAME_CKPT_A, &a), rb = slot_read(e, SYNC_NAME_CKPT_B, &b);
  if (ra < 0) return ra;
  if (rb < 0) return rb;
  memset(&e->ck, 0, sizeof e->ck);
  e->next_slot = 0;
  if (ra == 0) best = &a;
  if (rb == 0 && (!best || b.seq > a.seq)) best = &b;
  if (best) { e->ck = *best; e->next_slot = best == &a; }
  e->loaded = 1;
  return 0;
}

static void ckpt_save(SyncEngine *e) {
  uint8_t buf[128];
  if (e->ck.seq == 0xffffffffu) return;
  e->ck.seq++;
  ckpt_encode(&e->ck, buf);
  if (st_write(e, e->next_slot ? SYNC_NAME_CKPT_B : SYNC_NAME_CKPT_A, buf, sizeof buf) == 0) e->next_slot ^= 1;
}

static void ckpt_reset(SyncEngine *e) {
  memset(&e->ck.offset, 0, sizeof e->ck - offsetof(SyncCkpt, offset));
  e->resets++;
  ckpt_save(e);
}

static void status_write(SyncEngine *e) {
  char buf[STATUS_LEN];
  SBuf s;
  sb_init(&s, buf, sizeof buf - 1);
  sb_str(&s, "{\"stage\":\""); sb_str(&s, e->stage);
  sb_str(&s, "\",\"result\":\""); sb_str(&s, e->result);
  sb_str(&s, "\",\"errno\":"); sb_u64(&s, (uint64_t)(e->last_errno < 0 ? -e->last_errno : e->last_errno));
  sb_str(&s, ",\"http\":"); sb_u64(&s, (uint64_t)e->last_http);
  sb_str(&s, ",\"offset\":"); sb_u64(&s, e->ck.offset);
  sb_str(&s, ",\"acked\":"); sb_u64(&s, e->ck.acked);
  sb_str(&s, ",\"skipped_lines\":"); sb_u64(&s, (uint64_t)e->skipped_lines + e->trail_lines);
  sb_str(&s, ",\"skipped_bytes\":"); sb_u64(&s, (uint64_t)e->skipped_bytes + e->trail_bytes);
  sb_str(&s, ",\"held_bytes\":"); sb_u64(&s, e->held_bytes);
  sb_str(&s, ",\"resets\":"); sb_u64(&s, e->resets);
  sb_str(&s, ",\"attempts\":"); sb_u64(&s, e->attempts);
  sb_str(&s, ",\"conflict\":\""); sb_str(&s, e->conflict); sb_str(&s, "\"}");
  if (s.ovf) { sb_init(&s, buf, sizeof buf - 1); sb_str(&s, "{\"stage\":\"status\",\"result\":\"retry\"}"); }
  while (s.len < STATUS_LEN - 1) buf[s.len++] = ' ';
  buf[STATUS_LEN - 1] = '\n';
  (void)st_write(e, SYNC_NAME_STATUS, buf, STATUS_LEN);
}

static void set(SyncEngine *e, const char *stage, const char *result, int err) {
  strncpy(e->stage, stage, sizeof e->stage - 1);
  strncpy(e->result, result, sizeof e->result - 1);
  e->last_errno = err < 0 ? -err : err;
}

static SyncRun retry(SyncEngine *e, const char *stage, int err) {
  SyncRun r;
  e->backoff_ms = e->backoff_ms ? (e->backoff_ms > SYNC_BACKOFF_MAX_MS / 2 ? SYNC_BACKOFF_MAX_MS : e->backoff_ms * 2) : SYNC_BACKOFF_MIN_MS;
  e->attempts++;
  set(e, stage, "retry", err);
  r.outcome = SYNC_RETRY; r.delay_ms = e->backoff_ms;
  return r;
}

static SyncRun blocked(SyncEngine *e, const char *stage, int err) {
  SyncRun r;
  set(e, stage, "blocked", err);
  r.outcome = SYNC_BLOCKED; r.delay_ms = SYNC_BLOCKED_MS;
  return r;
}

static SyncRun idle(SyncEngine *e, const char *result) {
  SyncRun r;
  set(e, "done", result, 0);
  r.outcome = SYNC_IDLE; r.delay_ms = SYNC_IDLE_MS;
  return r;
}

static SyncRun run(SyncEngine *e) {
  SyncRun r;
  uint64_t t0 = e->plat.now_ms(e->plat.ctx), size;
  uint32_t sent = 0;
  int rc;
  e->last_http = 0; e->last_errno = 0; e->conflict[0] = 0;
  if (sync_cfg_validate(&e->cfg)) return blocked(e, "cfg", EINVAL);
  if (!e->loaded && (rc = ckpt_load(e)) < 0) return retry(e, "ckpt", rc);
  rc = e->plat.file_size(e->plat.ctx, e->cfg.journal, &size);
  if (rc == -ENOENT) return idle(e, "no_journal");
  if (rc < 0) return retry(e, "size", rc);
  if (e->ck.offset > 0) {
    rc = spool_verify(e, size, e->ck.offset, e->ck.anchor_len, e->ck.anchor_sha, e->ck.tail_len, e->ck.tail_sha, e->vhead, e->vtail);
    if (rc < 0) return retry(e, "anchor", rc);
    if (rc == 1) ckpt_reset(e);
  }
  for (unsigned nb = 0; nb < SYNC_MAX_BATCHES_PER_RUN; nb++) {
    Batch b;
    char id[48], hex[65], ahex[65];
    uint8_t *req;
    size_t req_len, resp_len = 0;
    uint64_t now = e->plat.now_ms(e->plat.ctx), dl;
    int http, ack;
    if (now - t0 >= SYNC_RUN_TIMEOUT_MS) {
      if (sent) { set(e, "send", "ok", 0); r.outcome = SYNC_MORE; r.delay_ms = 1000; return r; }
      return retry(e, "send", ETIMEDOUT);
    }
    if ((rc = spool_scan(e, &b)) < 0) return retry(e, "scan", rc);
    e->trail_lines = b.sk_lines - b.sk_lines_in; e->trail_bytes = b.sk_bytes - b.sk_bytes_in;
    if (b.count == 0) return idle(e, sent ? "ok" : "idle");
    {
      SBuf s;
      sha256_hex(b.anchor_sha, ahex);
      sha256_hex(b.body_sha, hex);
      sb_init(&s, id, sizeof id);
      memcpy(id, ahex, 16); id[16] = 0; s.len = 16;
      sb_ch(&s, '-'); sb_u64(&s, b.start); sb_ch(&s, '-'); sb_u64(&s, b.count);
    }
    if (!(req_len = http_build(e, &b, id, hex, &req))) return blocked(e, "req", EMSGSIZE);
    if (!e->net_up) {
      if ((rc = e->plat.net_open(e->plat.ctx)) < 0) return retry(e, "net", rc);
      e->net_up = 1;
    }
    dl = now + SYNC_EXCHANGE_TIMEOUT_MS;
    if (dl > t0 + SYNC_RUN_TIMEOUT_MS) dl = t0 + SYNC_RUN_TIMEOUT_MS;
    rc = e->plat.exchange(e->plat.ctx, e->cfg.ip, e->cfg.port, req, req_len, e->resp, sizeof e->resp, &resp_len, dl);
    if (rc < 0) return retry(e, "send", rc);
    if (resp_len > sizeof e->resp) return retry(e, "send", EIO);
    ack = http_check(e->resp, resp_len, id, hex, b.count, &http, e->conflict);
    e->last_http = http;
    if (ack == 2) return blocked(e, "ack", 0);
    if (ack) return retry(e, "ack", 0);
    /* Verified ack. Check the file still holds the exact bytes snapshotted before the exchange. */
    rc = e->plat.file_size(e->plat.ctx, e->cfg.journal, &size);
    if (rc == -ENOENT) rc = 1;
    else if (rc == 0) rc = spool_verify(e, size, b.end, b.anchor_len, b.anchor_sha, b.tail_len, b.tail_sha, NULL, NULL);
    if (rc < 0) return retry(e, "verify", rc);
    e->backoff_ms = 0; e->attempts = 0;
    sent++;
    if (rc == 1) {
      /* journal changed under the exchange: never bind this offset to the new bytes, replay from 0 */
      ckpt_reset(e);
      continue;
    }
    e->ck.offset = b.end;
    e->ck.acked += b.count;
    e->ck.anchor_len = b.anchor_len; e->ck.tail_len = b.tail_len;
    memcpy(e->ck.anchor_sha, b.anchor_sha, 32); memcpy(e->ck.tail_sha, b.tail_sha, 32);
    memcpy(e->vhead, b.ahead, b.anchor_len); memcpy(e->vtail, b.atail, b.tail_len);
    e->skipped_lines += b.sk_lines_in; e->skipped_bytes += b.sk_bytes_in;
    ckpt_save(e);
  }
  set(e, "send", "ok", 0);
  r.outcome = SYNC_MORE; r.delay_ms = 1000;
  return r;
}

SyncRun sync_run_once(SyncEngine *e) {
  SyncRun r = run(e);
  if (e->net_up) { e->plat.net_close(e->plat.ctx); e->net_up = 0; }
  status_write(e);
  return r;
}
