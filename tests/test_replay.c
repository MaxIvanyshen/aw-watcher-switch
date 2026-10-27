#include <errno.h>

#include "rig.h"
#include "t.h"

#define G1 0x0100000000001000ull
#define G2 0x0100000000002000ull

typedef enum { SRC_AUTO, SRC_LIVE, SRC_DUMP } Src;
typedef struct { Src src; int watched; } ReplayOpts;
int replay_run(FILE *in, FILE *out, FILE *err, const ReplayOpts *o);

static void ev_at(Rig *r, int64_t t, RawEvent e) { rig_tick(r, t); rig_push(r, e); rig_settle(r); }

static char *replay_text(const char *diag, size_t n, Src src, int watched) {
  FILE *in = tmpfile(), *out = tmpfile();
  fwrite(diag, 1, n, in);
  ReplayOpts o = {src, watched};
  replay_run(in, out, NULL, &o);
  long len = ftell(out);
  char *buf = calloc(1, (size_t)len + 1);
  rewind(out);
  if (fread(buf, 1, (size_t)len, out) != (size_t)len) abort();
  fclose(in); fclose(out);
  return buf;
}

static int count_lines(const char *s, const char *needle) {
  int c = 0;
  for (; (s = strstr(s, needle)) != NULL; s++) c++;
  return c;
}

static void scenario(Rig *r) {
  r->env.diag_flag = 1;
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  ev_at(r, 1050, ev_app(G1, AE_OUT_OF_FOCUS, 1050));
  ev_at(r, 1060, ev_account(1060));
  ev_at(r, 1070, ev_app(G2, AE_IN_FOCUS, 1070));
  ev_at(r, 1100, ev_power(1, 1100));
  ev_at(r, 1110, ev_app(G1, AE_IN_FOCUS, 1110));
  ev_at(r, 1150, ev_app(G2, AE_IN_FOCUS, 1150));
  ev_at(r, 1190, ev_app(G2, AE_EXIT, 1190));
  rig_at(r, 40000);
  ev_at(r, 40000, ev_app(G1, AE_IN_FOCUS, 40000));
  ev_at(r, 40040, ev_app(G1, AE_OUT_OF_FOCUS, 40040));
}

/* Same sessions as the engine journal: pid, start, why and (when the end was observed) end. */
static int same_sessions(const Journal_View *v, const char *replay) {
  const char *p = replay;
  for (int i = 0; i < v->n_valid; i++) {
    if (!rec_is_session(v->line[i])) continue;
    const char *nl = strchr(p, '\n');
    if (!nl) return 0;
    char line[JSONL_MAX + 1];
    size_t n = (size_t)(nl - p);
    memcpy(line, p, n); line[n] = 0;
    p = nl + 1;
    while (!rec_is_session(line) && *p) {   /* skip gap lines */
      nl = strchr(p, '\n'); n = (size_t)(nl - p); memcpy(line, p, n); line[n] = 0; p = nl + 1;
    }
    char a[24], b[24], pa[24], pb[24];
    uint64_t sa = 0, sb = 0, ea = 0, eb = 0;
    rec_str(v->line[i], "why", a, sizeof a); rec_str(line, "why", b, sizeof b);
    rec_str(v->line[i], "pid", pa, sizeof pa); rec_str(line, "pid", pb, sizeof pb);
    rec_num(v->line[i], "ss", &sa); rec_num(line, "ss", &sb);
    rec_num(v->line[i], "es", &ea); rec_num(line, "es", &eb);
    if (strcmp(a, b) || strcmp(pa, pb) || sa != sb) return 0;
    if (!rec_is_null(v->line[i], "eu") && ea != eb) return 0;
  }
  return 1;
}

static void replay_of_live_diag_matches_the_engine(void) {
  Rig *r = rig_new();
  scenario(r);
  const char *fixture = getenv("AW_WRITE_FIXTURE");   /* regenerates tests/fixtures/synthetic-diag.jsonl */
  if (fixture) {
    FILE *f = fopen(fixture, "w");
    if (f) { fwrite(r->env.diag, 1, r->env.diag_len, f); fclose(f); }
  }
  Journal_View v;
  jv_load(r, &v);
  CHECK(v.sessions >= 4);
  char *out = replay_text(r->env.diag, r->env.diag_len, SRC_AUTO, -1);
  CHECK_EQ(count_lines(out, "\"t\":\"session\""), v.sessions);
  CHECK(same_sessions(&v, out));
  CHECK(strstr(out, "\"why\":\"power\"") && strstr(out, "\"why\":\"superseded\""));
  free(out);
  jv_free(&v);
  rig_del(r);
}

static void replay_of_a_dump_is_unwatched_and_conservative(void) {
  Rig *r = rig_new();
  for (int i = 0; i < 4; i++) rig_push(r, ev_account(10));
  rig_push(r, ev_app(G1, AE_IN_FOCUS, 100));
  rig_push(r, ev_app(G1, AE_OUT_OF_FOCUS, 5000));
  rig_push(r, ev_app(G2, AE_IN_FOCUS, 5010));
  rig_push(r, ev_app(G2, AE_OUT_OF_FOCUS, 5020));
  rig_at(r, 6000); rig_settle(r);
  r->env.diag_flag = 1;
  ffs_set(&r->fs, AW_PATH_DIAG_FLAG, "dump_all", 8);
  rig_settle(r);
  char *out = replay_text(r->env.diag, r->env.diag_len, SRC_AUTO, -1);
  CHECK_EQ(count_lines(out, "\"t\":\"session\""), 2);
  CHECK_EQ(count_lines(out, "\"sem\":\"unc\""), 1);   /* 4900 s nobody observed */
  CHECK_EQ(count_lines(out, "\"sem\":\"ok\""), 1);
  free(out);
  out = replay_text(r->env.diag, r->env.diag_len, SRC_DUMP, 1);
  CHECK_EQ(count_lines(out, "\"sem\":\"ok\""), 2);
  free(out);
  rig_del(r);
}

static void replay_marks_resets_and_skips_crash_repeats(void) {
  Rig *r = rig_new();
  r->env.diag_flag = 1;
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_app(G1, AE_IN_FOCUS, 1000));
  rig_tick(r, 1020);
  r->fs.crash_at = -1;
  ffs_crash_plan(&r->fs, (int)r->fs.ops + 1, CM_BEFORE, &r->jb);
  ev_at(r, 1030, ev_app(G1, AE_OUT_OF_FOCUS, 1030));        /* crash after the append: the event is replayed after the restart */
  CHECK_EQ(r->crashes, 1);
  fp_reset(r->pdm);
  rig_push(r, ev_init(1040));
  rig_tick(r, 1040);
  char *out = replay_text(r->env.diag, r->env.diag_len, SRC_LIVE, -1);
  CHECK_EQ(count_lines(out, "\"t\":\"session\""), 1);
  CHECK_EQ(count_lines(out, "\"why\":\"log_reset\""), 1);
  free(out);
  rig_del(r);
}

static void replay_ignores_garbage_lines(void) {
  const char *junk = "not json\n{\"d\":\"raw\",\"k\":\"live\",\"i\":1,\"hex\":\"zz\"}\n{\"d\":\"boot\"}\n";
  char *out = replay_text(junk, strlen(junk), SRC_AUTO, -1);
  CHECK_EQ((long long)strlen(out), 0);
  free(out);
}

int main(void) {
  RUN(replay_of_live_diag_matches_the_engine);
  RUN(replay_of_a_dump_is_unwatched_and_conservative);
  RUN(replay_marks_resets_and_skips_crash_repeats);
  RUN(replay_ignores_garbage_lines);
  return t_done("replay");
}
