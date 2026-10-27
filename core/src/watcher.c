#include "watcher.h"

#include <errno.h>
#include <string.h>

#include "diag.h"
#include "journal.h"
#include "paths.h"
#include "state.h"
#include "util.h"

#define BATCH 32
#define MAX_BATCHES 64
#define POLL_MS 5000u
#define BUSY_MS 100u
#define BACKOFF_MIN_MS 5000u
#define BACKOFF_MAX_MS 300000u
#define HEARTBEAT_S 60u
#define RANGE_HEADROOM (2 * BATCH + 2)
#define TAIL_PROBE_MAX 64

struct Watcher {
  PdmSrc pdm; void *pu;
  Fs fs; void *fu;
  Env env; void *eu;
  Journal j;
  Diag d;
  DiagStats stats;
  PState st;
  uint32_t seq;
  int next_slot;
  const char *mode;
  uint8_t ready, fresh_why, check_anchor, st_dirty, dump_done, boot_logged, failed, have_range, now_ok;
  int32_t last_start, last_end;
  uint64_t now, last_save_s, last_stats_s;
  uint8_t src_now[16];
  uint32_t backoff_ms;
  char lbuf[JSONL_MAX + 2];
  RawEvent batch[BATCH];
};

_Static_assert(sizeof(struct Watcher) <= WATCHER_STORAGE_BYTES, "raise WATCHER_STORAGE_BYTES in watcher.h");
_Static_assert(_Alignof(struct Watcher) <= WATCHER_ALIGN, "Watcher alignment exceeds WATCHER_ALIGN");
_Static_assert(sizeof(RawEvent) >= PDM_EVENT_SIZE, "RawEvent too small");

size_t watcher_size(void) { return sizeof(struct Watcher); }

static int io_fail(Watcher *w, const char *op, int rc) {
  w->backoff_ms = w->backoff_ms ? (w->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : w->backoff_ms * 2) : BACKOFF_MIN_MS;
  w->failed = 1;
  w->stats.io_err++;
  diag_io(&w->d, op, rc, w->backoff_ms);
  return rc;
}

/* ---- state and journal writes ---- */

static int state_commit(Watcher *w, const PState *s) {
  int rc = state_save(&w->fs, w->fu, s, w->seq + 1, w->next_slot);
  if (rc) return io_fail(w, "state", rc);
  w->seq++;
  w->next_slot ^= 1;
  w->st_dirty = 0;
  w->last_save_s = w->now_ok ? w->now : w->last_save_s;
  return 0;
}

static void state_commit_soft(Watcher *w) {
  if (state_commit(w, &w->st)) w->st_dirty = 1;
}

static int emit_session(Watcher *w, uint32_t ep, int32_t ev, const SessionRec *r) {
  Key k = {ep, ev, SUB_SESSION};
  size_t n = jsonl_session(w->lbuf + 1, JSONL_MAX, &k, r);
  if (n == 0) return io_fail(w, "encode", EINVAL);
  int rc = journal_emit(&w->j, &k, w->lbuf, n);
  return rc ? io_fail(w, "append", rc) : 0;
}

static int emit_pending(Watcher *w) {
  if (!w->st.pend_why) return 0;
  Key k = {w->st.ep, w->st.cursor, SUB_GAP};
  size_t n = jsonl_gap(w->lbuf + 1, JSONL_MAX, &k, (GapWhy)w->st.pend_why, w->st.pend_from, w->st.pend_to);
  if (n == 0) return io_fail(w, "encode", EINVAL);
  int rc = journal_emit(&w->j, &k, w->lbuf, n);
  if (rc) return io_fail(w, "append", rc);
  w->st.pend_why = 0;
  w->st_dirty = 1;
  return 0;
}

/* Epoch change. The caller has already appended any closing record for the old epoch.
 * State goes first, with the gap record pending inside it, so a crash can delay the gap but never drop it. */
static int epoch_commit(Watcher *w, PState *n, GapWhy why, int32_t from, int32_t to) {
  n->pend_why = (uint8_t)why; n->pend_from = from; n->pend_to = to;
  int rc = state_commit(w, n);
  if (rc) return rc;
  w->st = *n;
  w->stats.epochs++;
  diag_tr(&w->d, "epoch", gap_why_name(why), n->ep, from, to);
  return 0;
}

/* ---- pdm access ---- */

static int read_range(Watcher *w, int32_t *start, int32_t *end, int32_t *total_out) {
  int32_t total = 0;
  *start = *end = 0;
  int rc = w->pdm.range(w->pu, &total, start, end);
  diag_range(&w->d, rc, total, *start, *end);
  if (rc) return io_fail(w, "range", rc);
  if (total < 0 || *start < 0 || *start > *end || *end > INT32_MAX - RANGE_HEADROOM) {
    diag_tr(&w->d, "range_invalid", "ignored", w->st.ep, *start, *end);
    return io_fail(w, "range", EPROTO);
  }
  *total_out = total;
  return 0;
}

static int query(Watcher *w, int32_t idx, RawEvent *out, int32_t max, int32_t *n, int32_t tail) {
  *n = 0;
  int rc = w->pdm.query(w->pu, idx, out, max, n);
  if (rc && idx >= tail) {
    diag_probe(&w->d, idx, rc, *n);
    *n = 0;
    return 0;
  }
  if (rc) return io_fail(w, "query", rc);
  if (*n < 0 || *n > max) return io_fail(w, "query", EPROTO);
  return 0;
}

/* First index where query returns nothing. end_entry_index may be inclusive or exclusive (E6), so probe from end-1. */
static int find_tail(Watcher *w, int32_t start, int32_t end, int32_t total, int32_t *tail) {
  /* When the count agrees with one of the two end conventions, it disambiguates
   * the last existing entry. Never mistake an error on that entry for EOF. */
  int64_t counted_tail = (int64_t)start + total;
  int32_t eof_floor = counted_tail == end || counted_tail == (int64_t)end + 1 ? (int32_t)counted_tail : end;
  int32_t idx = end - 1 > start ? end - 1 : start;
  for (int i = 0; i < TAIL_PROBE_MAX; i++, idx++) {
    RawEvent ev;
    int32_t n = -1;
    int rc = w->pdm.query(w->pu, idx, &ev, 1, &n);
    diag_probe(&w->d, idx, rc, n);
    if (rc) {
      if (idx >= eof_floor) { *tail = idx; return 0; }
      return io_fail(w, "probe", rc);
    }
    if (n < 0 || n > 1) return io_fail(w, "probe", EPROTO);
    if (n == 0) { *tail = idx; return 0; }
  }
  return io_fail(w, "probe", EPROTO);
}

static void fetch_anchor(Watcher *w, PState *n, int32_t start) {
  RawEvent ev;
  int32_t cnt;
  n->anchor_ok = 0;
  if (n->cursor - 1 < start) return;
  if (w->pdm.query(w->pu, n->cursor - 1, &ev, 1, &cnt) == 0 && cnt == 1) {
    n->anchor = crc32_of(ev.b, PDM_EVENT_SIZE);
    n->anchor_ok = 1;
  }
}

/* ---- recovery (3.5) ---- */

static int recover(Watcher *w) {
  PState s;
  uint32_t seq = 0;
  int slot = 0;
  StateLoad sl = state_load(&w->fs, w->fu, &s, &seq, &slot);
  if (sl == ST_IOERR) return io_fail(w, "state_load", EIO);
  int rc = journal_sync(&w->j);
  if (rc) return io_fail(w, "journal_sync", rc);

  int journal_ahead = sl == ST_OK && w->j.last_ok && w->j.last.ep > s.ep;
  if (sl == ST_OK && !journal_ahead) {
    w->st = s; w->seq = seq; w->next_slot = slot;
    w->check_anchor = 1;
    w->mode = "resume";
  } else {
    /* Nothing was ever recorded (no journal): an unreadable state is a first run that crashed before its first save. */
    GapWhy why = !w->j.exists && sl != ST_OK ? GAP_FIRST_RUN : sl == ST_CORRUPT ? GAP_STATE_CORRUPT : GAP_STATE_LOST;
    memset(&w->st, 0, sizeof w->st);
    w->st.ep = w->j.last_ok ? w->j.last.ep + 1 : 1;
    if (sl == ST_OK) { w->seq = seq; w->next_slot = slot; } else { w->seq = 0; w->next_slot = 0; }
    w->fresh_why = (uint8_t)why;
    w->mode = gap_why_name(why);
  }
  w->ready = 1;
  return 0;
}

static int begin_fresh(Watcher *w, int32_t start, int32_t tail) {
  PState n = w->st;
  n.cursor = tail;
  fetch_anchor(w, &n, start);
  if (w->now_ok) { memcpy(n.src, w->src_now, 16); n.src_ok = 1; }
  int rc = epoch_commit(w, &n, (GapWhy)w->fresh_why, tail, tail);
  if (rc) return rc;
  w->fresh_why = 0;
  w->check_anchor = 0;
  return emit_pending(w);
}

/* ---- log continuity (3.3) ---- */

static int log_break(Watcher *w, GapWhy why, int32_t start) {
  PState n = w->st;
  SessionRec r;
  if (tracker_close_unc(&n.tr, WHY_LOST, &r)) {
    int rc = emit_session(w, w->st.ep, w->st.cursor, &r);
    if (rc) return rc;
  }
  tracker_reset(&n.tr);
  n.ep = w->st.ep + 1;
  n.cursor = start;
  n.anchor_ok = 0;
  int rc = epoch_commit(w, &n, why, w->st.cursor, start);
  if (rc) return rc;
  w->check_anchor = 0;
  return emit_pending(w);
}

static int check_clock_source(Watcher *w) {
  if (!w->now_ok) return 0;
  if (!w->st.src_ok) {
    memcpy(w->st.src, w->src_now, 16);
    w->st.src_ok = 1;
    w->st_dirty = 1;
    return 0;
  }
  if (memcmp(w->st.src, w->src_now, 16) == 0) return 0;
  PState n = w->st;
  SessionRec r;
  if (tracker_close_unc(&n.tr, WHY_CLOCK_RESET, &r)) {
    int rc = emit_session(w, w->st.ep, w->st.cursor, &r);
    if (rc) return rc;
  }
  tracker_reset(&n.tr);
  n.ep = w->st.ep + 1;
  memcpy(n.src, w->src_now, 16);
  int rc = epoch_commit(w, &n, GAP_STEADY_RESET, w->st.cursor, w->st.cursor);
  if (rc) return rc;
  return emit_pending(w);
}

static int check_continuity(Watcher *w, int32_t start, int32_t end, int32_t tail) {
  int32_t cursor = w->st.cursor;
  int rc = 0;
  if (w->have_range && (end < w->last_end || start < w->last_start)) w->check_anchor = 1;
  w->last_start = start; w->last_end = end; w->have_range = 1;
  if (cursor < start) return log_break(w, GAP_LOG_DROPPED, start);
  if (cursor > tail) return log_break(w, GAP_LOG_RESET, start);
  if (w->check_anchor && w->st.anchor_ok && cursor > start) {
    RawEvent ev;
    int32_t n;
    rc = query(w, cursor - 1, &ev, 1, &n, tail);
    if (rc) return rc;
    if (n == 0 || crc32_of(ev.b, PDM_EVENT_SIZE) != w->st.anchor) return log_break(w, GAP_LOG_RESET, start);
  }
  w->check_anchor = 0;
  return 0;
}

/* ---- event transaction (3.4) ---- */

static void count_note(Watcher *w, const TrackOut *o) {
  w->stats.events++;
  if (o->note == NOTE_DUP_FOCUS) w->stats.dup_focus++;
  if (o->note >= NOTE_DUP_FOCUS) w->stats.ignored++;
}

static int process_event(Watcher *w, const RawEvent *raw) {
  int32_t idx = w->st.cursor;
  Event e;
  TrackOut o;
  PState n = w->st;
  decode_event(raw, &e);
  tracker_step(&n.tr, &e, &o);
  if (o.has_rec) {
    int rc = emit_session(w, w->st.ep, idx, &o.rec);
    if (rc) return rc;
  }
  n.cursor = idx + 1;
  n.anchor = crc32_of(raw->b, PDM_EVENT_SIZE);
  n.anchor_ok = 1;
  if (o.bump) {
    n.ep = w->st.ep + 1;
    int rc = epoch_commit(w, &n, GAP_STEADY_RESET, idx, idx);
    if (rc) return rc;
    count_note(w, &o);
    diag_raw(&w->d, "live", idx, raw, &e, &o);
    return emit_pending(w);
  }
  w->st = n;
  count_note(w, &o);
  diag_raw(&w->d, "live", idx, raw, &e, &o);
  w->st_dirty = 1;
  if (o.has_rec) state_commit_soft(w);
  return 0;
}

static int read_loop(Watcher *w, int *busy, int32_t tail) {
  for (int b = 0; b < MAX_BATCHES; b++) {
    int32_t n;
    int rc = query(w, w->st.cursor, w->batch, BATCH, &n, tail);
    if (rc) return rc;
    if (n == 0) return 0;
    for (int32_t i = 0; i < n; i++) {
      rc = process_event(w, &w->batch[i]);
      if (rc) return rc;
    }
    if (w->st_dirty && !w->failed) state_commit_soft(w);
  }
  *busy = 1;
  return 0;
}

/* ---- diag ---- */

int watcher_dump_all(Watcher *w) {
  if (!w) return EINVAL;
  uint8_t was_on = w->d.on;
  w->d.on = !w->d.dead;
  int32_t start, end, total;
  int rc = read_range(w, &start, &end, &total);
  int32_t tail = 0;
  if (!rc) rc = find_tail(w, start, end, total, &tail);
  Tracker scratch;
  tracker_reset(&scratch);
  for (int32_t idx = start; rc == 0 && diag_active(&w->d); ) {
    int32_t n;
    rc = query(w, idx, w->batch, BATCH, &n, tail);
    if (rc || n == 0) break;
    for (int32_t i = 0; i < n; i++, idx++) {
      Event e;
      TrackOut o;
      decode_event(&w->batch[i], &e);
      tracker_step(&scratch, &e, &o);
      diag_raw(&w->d, "dump", idx, &w->batch[i], &e, &o);
    }
  }
  w->d.on = was_on;
  return rc;
}

static void diag_flag(Watcher *w) {
  if (!diag_active(&w->d)) return;
  if (!w->boot_logged) {
    diag_boot(&w->d, w->mode ? w->mode : "?", w->st.ep, w->st.cursor);
    w->boot_logged = 1;
  }
  char buf[16];
  size_t n = 0;
  int rc = w->fs.read_file(w->fu, AW_PATH_DIAG_FLAG, buf, sizeof buf, &n);
  int want = rc == 0 && n >= 8 && memcmp(buf, "dump_all", 8) == 0;
  if (!want) { w->dump_done = 0; return; }
  if (w->dump_done) return;
  w->dump_done = 1;
  watcher_dump_all(w);
  w->fs.write_file(w->fu, AW_PATH_DIAG_FLAG, "", 0);
}

/* ---- poll ---- */

static int poll_once(Watcher *w, int *busy) {
  int rc;
  if (!w->ready && (rc = recover(w)) != 0) return rc;
  diag_flag(w);
  if ((rc = emit_pending(w)) != 0) return rc;
  int32_t start, end, total;
  if ((rc = read_range(w, &start, &end, &total)) != 0) return rc;
  int32_t tail;
  if ((rc = find_tail(w, start, end, total, &tail)) != 0) return rc;
  if (w->fresh_why) {
    if ((rc = begin_fresh(w, start, tail)) != 0) return rc;
  } else {
    if ((rc = check_clock_source(w)) != 0) return rc;
  }
  if ((rc = check_continuity(w, start, end, tail)) != 0) return rc;
  if ((rc = read_loop(w, busy, tail)) != 0) return rc;

  if (w->now_ok) {
    tracker_touch(&w->st.tr, w->now);
    if (w->st.tr.open && w->now - w->last_save_s >= HEARTBEAT_S) w->st_dirty = 1;
  }
  if (w->st_dirty && !w->failed) state_commit_soft(w);
  if (w->now_ok && diag_active(&w->d) && w->now - w->last_stats_s >= HEARTBEAT_S) {
    diag_stats(&w->d, &w->stats);
    w->last_stats_s = w->now;
  }
  return 0;
}

int watcher_poll(Watcher *w, uint32_t *next_sleep_ms) {
  uint32_t scratch;
  if (!w) return EINVAL;
  if (!next_sleep_ms) next_sleep_ms = &scratch;
  w->failed = 0;
  diag_begin(&w->d);
  int64_t secs = 0;
  w->now_ok = w->env.steady(w->eu, &secs, w->src_now) == 0 && secs >= 0;
  w->now = w->now_ok ? (uint64_t)secs : 0;
  if (w->now_ok && w->now < w->last_save_s) w->last_save_s = w->now;
  if (w->now_ok && w->now < w->last_stats_s) w->last_stats_s = w->now;
  int busy = 0;
  int rc = poll_once(w, &busy);
  if (rc && !w->failed) io_fail(w, "poll", rc);
  if (w->failed) *next_sleep_ms = w->backoff_ms;
  else { w->backoff_ms = 0; *next_sleep_ms = busy ? BUSY_MS : POLL_MS; }
  return 0;
}

int watcher_init(Watcher *w, PdmSrc pdm, void *pu, Fs fs, void *fu, Env env, void *eu) {
  if (!w || !pdm.range || !pdm.query || !fs.append_line || !fs.read_tail || !fs.write_file || !fs.read_file ||
      !env.steady || !env.diag_enabled || !env.diag_write)
    return EINVAL;
  memset(w, 0, sizeof *w);
  w->pdm = pdm; w->pu = pu; w->fs = fs; w->fu = fu; w->env = env; w->eu = eu;
  journal_init(&w->j, &w->fs, w->fu);
  diag_init(&w->d, &w->env, w->eu);
  recover(w);
  w->failed = 0;
  w->backoff_ms = 0;
  return 0;
}
