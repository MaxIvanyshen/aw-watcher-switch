#include <errno.h>

#include "rig.h"
#include "t.h"

#define G1 0x0100000000001000ull
#define G2 0x0100000000002000ull

static void ev_at(Rig *r, int64_t t, RawEvent e) { rig_tick(r, t); rig_push(r, e); rig_settle(r); }

static int count(const Rig *r, const char *needle) {
  int c = 0;
  for (const char *p = r->env.diag; p && (p = strstr(p, needle)) != NULL; p++) c++;
  return c;
}

static int all_lines_well_formed(const Rig *r) {
  const char *p = r->env.diag;
  const char *end = r->env.diag + r->env.diag_len;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl || nl - p > 512 || p[0] != '{' || nl[-1] != '}') return 0;
    p = nl + 1;
  }
  return 1;
}

static void diag_off_writes_nothing(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  ev_at(r, 1020, ev_app(G1, AE_OUT_OF_FOCUS, 1020));
  CHECK_EQ(r->env.diag_lines, 0);
  rig_del(r);
}

static void diag_records_every_consumed_event_with_raw_bytes(void) {
  Rig *r = rig_new();
  r->env.diag_flag = 1;
  rig_at(r, 1000); rig_settle(r);
  CHECK_EQ(count(r, "\"d\":\"boot\""), 1);
  CHECK(count(r, "\"d\":\"range\"") >= 1);
  CHECK(count(r, "\"d\":\"probe\"") >= 1);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  ev_at(r, 1005, ev_account(1005));
  ev_at(r, 1010, ev_raw(PT_APPLET, G1, 0x02, 0, AE_IN_FOCUS, 0, 1, 1, 1010));
  ev_at(r, 1020, ev_app(G1, AE_OUT_OF_FOCUS, 1020));
  CHECK_EQ(count(r, "\"d\":\"raw\""), 4);
  CHECK_EQ(count(r, "\"k\":\"live\""), 4);
  CHECK(count(r, "\"act\":\"open\"") == 1 && count(r, "\"act\":\"ign_account\"") == 1);
  CHECK(count(r, "\"act\":\"ign_applet_id\"") == 1 && count(r, "\"act\":\"close\",\"why\":\"out_of_focus\",\"sem\":\"ok\"") == 1);
  const char *p = strstr(r->env.diag, "\"d\":\"raw\"");
  const char *h = strstr(p, "\"hex\":\"");
  CHECK(h != NULL);
  const char *q = h + 7;
  size_t n = 0;
  while (q[n] != '"') n++;
  CHECK_EQ(n, 112);
  CHECK(strstr(r->env.diag, "abababababababababababababababab") != NULL);   /* account ids are in the raw dump: docs warn */
  CHECK(all_lines_well_formed(r));
  rig_del(r);
}

static void diag_can_be_enabled_without_a_restart(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  CHECK_EQ(r->env.diag_lines, 0);
  r->env.diag_flag = 1;
  ev_at(r, 1010, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
  CHECK_EQ(count(r, "\"d\":\"boot\""), 1);
  CHECK_EQ(count(r, "\"d\":\"raw\""), 1);
  r->env.diag_flag = 0;
  uint32_t n = r->env.diag_lines;
  ev_at(r, 1020, ev_app(G1, AE_IN_FOCUS, 1020));
  CHECK_EQ(r->env.diag_lines, n);
  rig_del(r);
}

static void snapshot_files(Rig *r, uint8_t *dst, size_t *len) {
  *len = 0;
  for (int i = 0; i < FF_FILES; i++)
    if (r->fs.files[i].used) { memcpy(dst + *len, r->fs.files[i].d, r->fs.files[i].n); *len += r->fs.files[i].n; }
}

static void dump_all_writes_the_whole_log_once_and_resets_the_flag(void) {
  Rig *r = rig_new();
  for (int i = 0; i < 40; i++) rig_push(r, ev_app(G1, i % 2 ? AE_OUT_OF_FOCUS : AE_IN_FOCUS, 10 + (uint64_t)i));
  rig_push(r, ev_account(60));
  rig_at(r, 1000); rig_settle(r);
  r->env.diag_flag = 1;
  ffs_set(&r->fs, AW_PATH_DIAG_FLAG, "dump_all\n", 9);
  rig_settle(r);
  CHECK_EQ(count(r, "\"k\":\"dump\""), 41);
  FFile *f = ffs_find(&r->fs, AW_PATH_DIAG_FLAG);
  CHECK(f && f->n == 0);
  rig_settle(r); rig_settle(r);
  CHECK_EQ(count(r, "\"k\":\"dump\""), 41);
  CHECK(all_lines_well_formed(r));
  rig_del(r);
}

static void dump_all_leaves_journal_and_state_alone(void) {
  Rig *r = rig_new();
  for (int i = 0; i < 10; i++) rig_push(r, ev_account(10 + (uint64_t)i));
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  uint8_t *before = malloc(FF_FILES * FF_CAP), *after = malloc(FF_FILES * FF_CAP);
  size_t nb, na;
  snapshot_files(r, before, &nb);
  uint32_t ops = r->fs.ops;
  CHECK_EQ(watcher_dump_all(r->w), 0);
  snapshot_files(r, after, &na);
  CHECK(nb == na && memcmp(before, after, nb) == 0);
  CHECK_EQ(r->fs.ops, ops);
  CHECK(count(r, "\"k\":\"dump\"") >= 11);
  free(before); free(after);
  CHECK(watcher_dump_all(NULL) != 0);
  rig_del(r);
}

static void dump_all_flag_that_cannot_be_rewritten_does_not_loop(void) {
  Rig *r = rig_new();
  for (int i = 0; i < 5; i++) rig_push(r, ev_account(10 + (uint64_t)i));
  rig_at(r, 1000); rig_settle(r);
  r->env.diag_flag = 1;
  ffs_set(&r->fs, AW_PATH_DIAG_FLAG, "dump_all", 8);
  r->fs.fault_at = (int)r->fs.ops; r->fs.fault_len = 1000; r->fs.fault_kind = FK_CLEAN;
  for (int i = 0; i < 5; i++) rig_poll(r);
  CHECK_EQ(count(r, "\"k\":\"dump\""), 5);
  rig_del(r);
}

static void diag_failure_disables_diag_not_recording(void) {
  Rig *r = rig_new();
  r->env.diag_flag = 1;
  r->env.diag_fail_after = 3;
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  ev_at(r, 1020, ev_app(G1, AE_OUT_OF_FOCUS, 1020));
  CHECK_EQ(r->env.diag_lines, 3);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  jv_free(&v);
  rig_del(r);
}

static void diag_is_capped_with_one_trunc_line(void) {
  Rig *r = rig_new();
  for (int i = 0; i < 8100; i++) rig_push(r, ev_account(10));
  rig_at(r, 1000); rig_settle(r);
  r->env.diag_flag = 1;
  ffs_set(&r->fs, AW_PATH_DIAG_FLAG, "dump_all", 8);
  rig_settle(r);
  CHECK_EQ(count(r, "\"d\":\"trunc\""), 1);
  CHECK(r->env.diag_len <= 2u * 1024u * 1024u + 64u);
  CHECK(r->env.diag_len > 2u * 1024u * 1024u - 1024u);
  uint32_t n = r->env.diag_lines;
  ev_at(r, 1010, ev_app(G1, AE_IN_FOCUS, 1010));
  CHECK_EQ(r->env.diag_lines, n);                           /* off until restart */
  rig_del(r);
}

static void diag_notes_io_errors_and_epochs(void) {
  Rig *r = rig_new();
  r->env.diag_flag = 1;
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  r->fs.fault_at = (int)r->fs.ops; r->fs.fault_len = 1; r->fs.fault_errno = EIO;
  ev_at(r, 1010, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
  CHECK(count(r, "\"d\":\"io\",\"op\":\"append\",\"rc\":5") >= 1);
  r->pdm->range_garbage = 1;
  rig_poll(r);
  CHECK(count(r, "\"what\":\"range_invalid\"") >= 1);
  r->pdm->range_garbage = 0;
  r->env.src[0] = 9;
  rig_settle(r);
  CHECK(count(r, "\"what\":\"epoch\",\"why\":\"steady_reset\"") == 1);
  CHECK(count(r, "\"d\":\"stats\"") >= 0);
  CHECK(all_lines_well_formed(r));
  rig_del(r);
}

int main(void) {
  RUN(diag_off_writes_nothing);
  RUN(diag_records_every_consumed_event_with_raw_bytes);
  RUN(diag_can_be_enabled_without_a_restart);
  RUN(dump_all_writes_the_whole_log_once_and_resets_the_flag);
  RUN(dump_all_leaves_journal_and_state_alone);
  RUN(dump_all_flag_that_cannot_be_rewritten_does_not_loop);
  RUN(diag_failure_disables_diag_not_recording);
  RUN(diag_is_capped_with_one_trunc_line);
  RUN(diag_notes_io_errors_and_epochs);
  return t_done("diag");
}
