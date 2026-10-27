#include <errno.h>

#include "rig.h"
#include "state.h"
#include "t.h"
#include "util.h"

#define G1 0x0100000000001000ull
#define G2 0x0100000000002000ull

static void ev_at(Rig *r, int64_t t, RawEvent e) { rig_tick(r, t); rig_push(r, e); rig_settle(r); }
#define FOCUS(r, pid, t) ev_at(r, t, ev_app(pid, AE_IN_FOCUS, (uint64_t)(t)))
#define UNFOCUS(r, pid, t) ev_at(r, t, ev_app(pid, AE_OUT_OF_FOCUS, (uint64_t)(t)))

static uint64_t num(const char *line, const char *f) { uint64_t v = ~0ull; rec_num(line, f, &v); return v; }

static Rig *started(void) {
  Rig *r = rig_new();
  rig_at(r, 1000);
  rig_settle(r);
  return r;
}

static void session(Rig *r, uint64_t pid, int64_t t) {
  FOCUS(r, pid, t);
  UNFOCUS(r, pid, t + 20);
}

static void append_failures_back_off_and_lose_nothing(void) {
  FaultKind kinds[] = {FK_CLEAN, FK_PARTIAL, FK_LANDED};
  for (int k = 0; k < 3; k++) {
    Rig *r = started();
    FOCUS(r, G1, 1000);
    rig_tick(r, 1010);
    r->fs.fault_at = (int)r->fs.ops; r->fs.fault_len = 3; r->fs.fault_kind = kinds[k]; r->fs.fault_errno = EIO;
    rig_push(r, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
    rig_poll(r);
    CHECK_EQ(r->last_sleep, 5000);
    if (kinds[k] != FK_LANDED) {
      rig_poll(r); CHECK_EQ(r->last_sleep, 10000);
      rig_poll(r); CHECK_EQ(r->last_sleep, 20000);
    }
    rig_settle(r);
    CHECK_EQ(r->last_sleep, 5000);
    Journal_View v;
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 1);
    CHECK(jv_keys_increasing(&v));
    CHECK(!v.unterminated);
    if (kinds[k] == FK_PARTIAL) CHECK(v.n_bad >= 1);
    else CHECK_EQ(v.n_bad, 0);
    jv_free(&v);
    session(r, G2, 1100);
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 2);
    jv_free(&v);
    rig_del(r);
  }
}

static void state_write_failure_is_not_fatal(void) {
  Rig *r = started();
  FOCUS(r, G1, 1000);
  rig_tick(r, 1010);
  r->fs.fault_at = (int)r->fs.ops + 1; r->fs.fault_len = 1; r->fs.fault_kind = FK_PARTIAL;   /* the state write after the append */
  ev_at(r, 1010, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  jv_free(&v);
  rig_boot(r);                                   /* stale state: replay must not duplicate */
  rig_settle(r);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  CHECK_EQ(v.n_bad, 0);
  jv_free(&v);
  session(r, G2, 1100);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  jv_free(&v);
  rig_del(r);
}

static void sd_outage_then_return_records_everything_once(void) {
  Rig *r = started();
  session(r, G1, 1000);
  r->fs.fault_at = (int)r->fs.ops; r->fs.fault_len = 8; r->fs.fault_kind = FK_CLEAN; r->fs.fault_errno = ENODEV;
  FOCUS(r, G2, 1100);
  UNFOCUS(r, G2, 1120);
  FOCUS(r, G1, 1130);
  UNFOCUS(r, G1, 1150);
  for (int i = 0; i < 12; i++) rig_poll(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 3);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  rig_del(r);
}

static void state_lost_keeps_the_epoch_moving_forward(void) {
  Rig *r = started();
  session(r, G1, 1000);
  FOCUS(r, G2, 1100);
  ffs_remove(&r->fs, AW_PATH_STATE_A);
  ffs_remove(&r->fs, AW_PATH_STATE_B);
  rig_boot(r);
  rig_settle(r);
  UNFOCUS(r, G2, 1120);                                    /* the open session was lost with the state */
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  int g = jv_find(&v, 1, "state_lost");
  CHECK(g > 0); CHECK_EQ(num(v.line[g], "ep"), 2);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  session(r, G1, 1200);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(num(v.line[v.n_valid - 1], "ep"), 2);
  jv_free(&v);
  rig_del(r);
}

static void both_slots_corrupt_marks_a_gap_once_and_keeps_the_files(void) {
  Rig *r = started();
  session(r, G1, 1000);
  ffs_set(&r->fs, AW_PATH_STATE_A, "garbage-a", 9);
  ffs_set(&r->fs, AW_PATH_STATE_B, "garbage-b", 9);
  rig_boot(r);
  rig_settle(r);
  CHECK(ffs_find(&r->fs, AW_PATH_STATE_B) != NULL);
  Journal_View v;
  jv_load(r, &v);
  CHECK(jv_find(&v, 0, "state_corrupt") > 0);
  int gaps = v.gaps;
  jv_free(&v);
  rig_boot(r);                                              /* no crash loop, no second marker */
  rig_boot(r);
  rig_settle(r);
  jv_load(r, &v);
  CHECK_EQ(v.gaps, gaps);
  jv_free(&v);
  session(r, G2, 1100);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  jv_free(&v);
  rig_del(r);
}

static void one_corrupt_slot_resumes_from_the_other(void) {
  Rig *r = started();
  session(r, G1, 1000);
  FFile *a = ffs_find(&r->fs, AW_PATH_STATE_A), *b = ffs_find(&r->fs, AW_PATH_STATE_B);
  FFile *newer = a->n && b->n ? (memcmp(a->d + 8, b->d + 8, 4) > 0 ? a : b) : (a->n ? a : b);
  newer->d[30] ^= 0x55;
  rig_boot(r);
  rig_settle(r);
  session(r, G2, 1100);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(v.gaps, 1);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  rig_del(r);
}

static void unknown_state_version_counts_as_corrupt(void) {
  Rig *r = started();
  session(r, G1, 1000);
  for (int i = 0; i < 2; i++) {
    FFile *f = ffs_find(&r->fs, i ? AW_PATH_STATE_B : AW_PATH_STATE_A);
    if (!f || !f->n) continue;
    f->d[4] = 2;
    uint32_t crc = crc32_of(f->d, f->n - 4);
    for (int k = 0; k < 4; k++) f->d[f->n - 4 + (size_t)k] = (uint8_t)(crc >> (8 * k));
  }
  rig_boot(r);
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK(jv_find(&v, 0, "state_corrupt") > 0);
  jv_free(&v);
  rig_del(r);
}

static void torn_journal_tail_is_fenced(void) {
  Rig *r = started();
  session(r, G1, 1000);
  FFile *j = ffs_find(&r->fs, AW_PATH_SESSIONS);
  memcpy(j->d + j->n, "{\"v\":1,\"t\":\"session\",\"ep\":1,\"ev\":99", 35);
  j->n += 35;
  rig_boot(r);
  session(r, G2, 1100);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(v.n_bad, 1);
  CHECK(!v.unterminated);
  jv_free(&v);
  rig_del(r);
}

static void zero_filled_journal_tail_is_fenced(void) {
  Rig *r = started();
  session(r, G1, 1000);
  FFile *j = ffs_find(&r->fs, AW_PATH_SESSIONS);
  memset(j->d + j->n, 0, 120);
  j->n += 120;
  rig_boot(r);
  session(r, G2, 1100);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(v.n_bad, 1);
  jv_free(&v);
  rig_del(r);
}

static void journal_without_trailing_newline_is_not_replayed_or_merged(void) {
  Rig *r = started();
  session(r, G1, 1000);
  FFile *j = ffs_find(&r->fs, AW_PATH_SESSIONS);
  j->n--;                                                   /* last record complete, newline lost */
  rig_boot(r);
  rig_settle(r);
  session(r, G2, 1100);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(v.n_bad, 0);
  jv_free(&v);
  rig_del(r);
}

static void journal_read_error_at_boot_consumes_nothing(void) {
  Rig *r = started();
  session(r, G1, 1000);
  rig_boot(r);
  strcpy(r->fs.read_err_path, "sessions.jsonl"); r->fs.read_err = EIO; r->fs.read_err_n = -1;
  rig_boot(r);
  uint8_t a0[200], b0[200], a1[200], b1[200];
  size_t an0, bn0, an1, bn1;
  snapshot_state(r, a0, &an0, b0, &bn0);
  FOCUS(r, G2, 1100);
  UNFOCUS(r, G2, 1120);
  rig_poll(r); rig_poll(r);
  CHECK(r->last_sleep >= 5000);
  snapshot_state(r, a1, &an1, b1, &bn1);
  CHECK(an0 == an1 && bn0 == bn1 && memcmp(a0, a1, an0) == 0 && memcmp(b0, b1, bn0) == 0);
  r->fs.read_err = 0;
  rig_settle(r);
  rig_poll(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  jv_free(&v);
  rig_del(r);
}

static void state_read_error_never_looks_like_corruption(void) {
  Rig *r = started();
  session(r, G1, 1000);
  strcpy(r->fs.read_err_path, "state.a"); r->fs.read_err = EIO; r->fs.read_err_n = -1;
  rig_boot(r);
  rig_poll(r); rig_poll(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(jv_find(&v, 0, "state_"), -1);
  jv_free(&v);
  r->fs.read_err = 0;
  rig_settle(r);
  session(r, G2, 1100);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(jv_find(&v, 0, "state_"), -1);
  jv_free(&v);
  rig_del(r);
}

static void journal_epoch_ahead_of_state_is_not_trusted(void) {
  Rig *r = started();
  FOCUS(r, G1, 1000);
  uint8_t a[200], b[200];
  size_t an, bn;
  snapshot_state(r, a, &an, b, &bn);
  rig_tick(r, 1010);
  r->env.src[0] = 7;                                         /* epoch 2 starts */
  r->env.steady = 40;
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK(jv_find(&v, 0, "steady_reset") > 0);
  jv_free(&v);
  if (an) ffs_set(&r->fs, AW_PATH_STATE_A, a, an); else ffs_remove(&r->fs, AW_PATH_STATE_A);   /* state rolled back across the epoch change */
  if (bn) ffs_set(&r->fs, AW_PATH_STATE_B, b, bn); else ffs_remove(&r->fs, AW_PATH_STATE_B);
  rig_boot(r);
  rig_settle(r);
  jv_load(r, &v);
  int g = jv_find(&v, 0, "state_lost");
  CHECK(g > 0);
  CHECK_EQ(num(v.line[g], "ep"), 3);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  rig_del(r);
}

static void rolled_back_state_replays_without_duplicates(void) {
  for (int open_at_snapshot = 0; open_at_snapshot < 2; open_at_snapshot++) {
    Rig *r = started();
    session(r, G1, 1000);
    if (open_at_snapshot) FOCUS(r, G2, 1050);
    uint8_t a[200], b[200];
    size_t an, bn;
    snapshot_state(r, a, &an, b, &bn);
    if (open_at_snapshot) UNFOCUS(r, G2, 1070);
    for (int i = 0; i < 5; i++) session(r, G1, 1100 + i * 100);
    Journal_View v0;
    jv_load(r, &v0);
    FFile *j = ffs_find(&r->fs, AW_PATH_SESSIONS);
    size_t size0 = j->n;
    if (an) ffs_set(&r->fs, AW_PATH_STATE_A, a, an); else ffs_remove(&r->fs, AW_PATH_STATE_A);
    if (bn) ffs_set(&r->fs, AW_PATH_STATE_B, b, bn); else ffs_remove(&r->fs, AW_PATH_STATE_B);
    rig_boot(r);
    rig_settle(r);
    Journal_View v1;
    jv_load(r, &v1);
    CHECK_EQ((long long)j->n, (long long)size0);               /* nothing new appended */
    CHECK_EQ(v1.n_valid, v0.n_valid);
    CHECK(jv_keys_increasing(&v1));
    int before = v0.sessions;
    jv_free(&v0); jv_free(&v1);
    session(r, G2, 2000);
    jv_load(r, &v1);
    CHECK_EQ(v1.sessions, before + 1);
    jv_free(&v1);
    rig_del(r);
  }
}

static void missing_journal_with_valid_state_just_continues(void) {
  Rig *r = started();
  session(r, G1, 1000);
  ffs_remove(&r->fs, AW_PATH_SESSIONS);
  rig_boot(r);
  session(r, G2, 1100);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  CHECK_EQ(v.n_bad, 0);
  jv_free(&v);
  rig_del(r);
}

int main(void) {
  RUN(append_failures_back_off_and_lose_nothing);
  RUN(state_write_failure_is_not_fatal);
  RUN(sd_outage_then_return_records_everything_once);
  RUN(state_lost_keeps_the_epoch_moving_forward);
  RUN(both_slots_corrupt_marks_a_gap_once_and_keeps_the_files);
  RUN(one_corrupt_slot_resumes_from_the_other);
  RUN(unknown_state_version_counts_as_corrupt);
  RUN(torn_journal_tail_is_fenced);
  RUN(zero_filled_journal_tail_is_fenced);
  RUN(journal_without_trailing_newline_is_not_replayed_or_merged);
  RUN(journal_read_error_at_boot_consumes_nothing);
  RUN(state_read_error_never_looks_like_corruption);
  RUN(journal_epoch_ahead_of_state_is_not_trusted);
  RUN(rolled_back_state_replays_without_duplicates);
  RUN(missing_journal_with_valid_state_just_continues);
  return t_done("faults");
}
