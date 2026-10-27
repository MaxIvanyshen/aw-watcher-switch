#include <errno.h>

#include "rig.h"
#include "state.h"
#include "t.h"
#include "tracker.h"

#define NPID 3
static const uint64_t PIDS[NPID] = {0x0100000000001000ull, 0x0100000000002000ull, 0x0100000000003000ull};

typedef struct {
  uint64_t s;
  uint64_t uid;                    /* unique user timestamp per pushed event */
  int64_t now;
  int chaos;                       /* mode B: log expiry/reset, bad ranges, state damage */
  struct { uint64_t pid, su, ss; } focus[512];
  int nfocus;
  RawEvent pushed[2048];
  int npushed;
} Gen;

static uint64_t rnd(Gen *g, uint64_t n) {
  g->s ^= g->s >> 12; g->s ^= g->s << 25; g->s ^= g->s >> 27;
  return (g->s * 0x2545F4914F6CDD1Dull) % n;
}

static void push(Rig *r, Gen *g, uint8_t pt, uint64_t pid, uint8_t ae, uint8_t pol, uint8_t appid, uint64_t ts) {
  RawEvent e = ev_raw(pt, pid, appid, pol, ae, (uint8_t)rnd(g, 4), ++g->uid, rnd(g, 2) ? 0 : rnd(g, 1u << 30), ts);
  if (g->npushed < 2048) g->pushed[g->npushed++] = e;
  if (pt == PT_APPLET && ae == AE_IN_FOCUS && appid == APPLET_ID_APPLICATION && pol != POLICY_NONE && g->nfocus < 512) {
    g->focus[g->nfocus].pid = pid; g->focus[g->nfocus].su = g->uid; g->focus[g->nfocus].ss = ts; g->nfocus++;
  }
  fp_push(r->pdm, &e);
}

static void gen_event(Rig *r, Gen *g) {
  uint64_t pid = PIDS[rnd(g, NPID)];
  uint64_t ts = (uint64_t)g->now;
  switch (rnd(g, 14)) {
  case 0: case 1: case 2: case 3: push(r, g, PT_APPLET, pid, AE_IN_FOCUS, POLICY_ALL, 1, ts); break;
  case 4: case 5: case 6: push(r, g, PT_APPLET, pid, AE_OUT_OF_FOCUS, POLICY_ALL, 1, ts); break;
  case 7: push(r, g, PT_APPLET, pid, rnd(g, 2) ? AE_EXIT : AE_EXIT6, POLICY_ALL, 1, ts); break;
  case 8: push(r, g, PT_POWER, 0, 0, 0, 0, ts); break;
  case 9: push(r, g, PT_INIT, 0, 0, 0, 0, ts); break;
  case 10: push(r, g, PT_ACCOUNT, pid, 0, 0, 0, ts); break;
  case 11: push(r, g, PT_APPLET, pid, AE_LAUNCH, POLICY_ALL, 1, ts); break;
  case 12: push(r, g, PT_APPLET, pid, (uint8_t)rnd(g, 7), POLICY_NONE, (uint8_t)rnd(g, 3), ts); break;
  default:
    if (rnd(g, 6) == 0 && g->now > 500) push(r, g, PT_APPLET, pid, AE_IN_FOCUS, POLICY_ALL, 1, ts - 400 + rnd(g, 100));   /* steady regression */
    else push(r, g, PT_APPLET, pid, AE_IN_FOCUS, POLICY_LOG_ONLY, 1, ts);
    break;
  }
}

static void arm_crash(Rig *r, Gen *g) {
  ffs_crash_plan(&r->fs, (int)r->fs.ops + (int)rnd(g, 14), (CrashMode)rnd(g, CM_COUNT), &r->jb);
}

static void gen_step(Rig *r, Gen *g) {
  switch (rnd(g, 16)) {
  case 0: case 1: case 2: case 3: case 4:
    g->now += 1 + (int64_t)rnd(g, 4); gen_event(r, g); rig_at(r, g->now); rig_poll(r); break;
  case 5: case 6: g->now += 1 + (int64_t)rnd(g, 25); rig_at(r, g->now); rig_poll(r); break;
  case 7: g->now += 5 + (int64_t)rnd(g, 100); rig_tick(r, g->now); break;
  case 8: g->now += 200 + (int64_t)rnd(g, 20000); rig_at(r, g->now); rig_poll(r); break;       /* suspend */
  case 9: rig_boot(r); break;
  case 10: arm_crash(r, g); break;
  case 11:
    r->fs.fault_at = (int)r->fs.ops + (int)rnd(g, 6); r->fs.fault_len = 1 + (int)rnd(g, 5);
    r->fs.fault_kind = (FaultKind)rnd(g, 3); r->fs.fault_errno = rnd(g, 2) ? EIO : ENODEV;
    break;
  case 12:
    strcpy(r->fs.read_err_path, rnd(g, 2) ? "sessions" : "state"); r->fs.read_err = EIO; r->fs.read_err_n = 1 + (int)rnd(g, 3);
    break;
  case 13: r->pdm->end_inclusive = (int)rnd(g, 2); break;
  case 14:
    if (g->chaos) {
      switch (rnd(g, 6)) {
      case 0: fp_expire(r->pdm, (int32_t)rnd(g, 6)); break;
      case 1: fp_reset(r->pdm); break;
      case 2: r->pdm->range_garbage = (int)rnd(g, 5); break;
      case 3: r->pdm->query_err = EIO; r->pdm->query_err_n = 1 + (int)rnd(g, 3); r->pdm->query_err_from = 0; break;
      case 4: { FFile *f = ffs_find(&r->fs, rnd(g, 2) ? AW_PATH_STATE_A : AW_PATH_STATE_B); if (f && f->n) f->d[rnd(g, f->n)] ^= (uint8_t)(1 + rnd(g, 255)); break; }
      default: ffs_remove(&r->fs, rnd(g, 2) ? AW_PATH_STATE_A : AW_PATH_STATE_B); break;
      }
    }
    break;
  default: rig_poll(r); break;
  }
}

static void finish(Rig *r) {
  ffs_clear_plans(&r->fs);
  r->pdm->range_garbage = 0; r->pdm->query_err = 0; r->env.steady_err = 0;
  r->env.steady += 5;
  for (int i = 0; i < 6; i++) { rig_poll(r); r->env.steady += 5; }
  for (int i = 0; i < 4; i++) rig_settle(r);
}

/* Replays the pushed events through the tracker. Only valid when the watcher saw every event. */
static int oracle(const Gen *g, SessionRec *out, int cap) {
  Tracker t;
  tracker_reset(&t);
  int n = 0;
  for (int i = 0; i < g->npushed; i++) {
    Event e;
    TrackOut o;
    decode_event(&g->pushed[i], &e);
    tracker_step(&t, &e, &o);
    if (o.has_rec && n < cap) out[n++] = o.rec;
  }
  return n;
}

static void check_no_phantoms(const Gen *g, const Journal_View *v, uint32_t seed) {
  for (int i = 0; i < v->n_valid; i++) {
    const char *l = v->line[i];
    if (!rec_is_session(l)) continue;
    uint64_t su = 0, ss = 0;
    char pid[24];
    rec_num(l, "su", &su); rec_num(l, "ss", &ss); rec_str(l, "pid", pid, sizeof pid);
    int found = 0, dup = 0;
    for (int k = 0; k < g->nfocus; k++)
      if (g->focus[k].su == su) {
        char want[24];
        snprintf(want, sizeof want, "0x%016llx", (unsigned long long)g->focus[k].pid);
        found = g->focus[k].ss == ss && strcmp(want, pid) == 0;
      }
    for (int j = 0; j < i; j++) { uint64_t o = 0; if (rec_is_session(v->line[j]) && rec_num(v->line[j], "su", &o) && o == su) dup = 1; }
    if (!found || dup) { fprintf(stderr, "seed %u: %s session: %s\n", seed, dup ? "duplicate" : "phantom", l); t_fail++; }
    t_checks++;
  }
}

static long tot_sessions[2], tot_gaps[2], tot_crashes[2], tot_bad[2], tot_runs[2];

static void run_seed(uint32_t seed, int chaos, int steps) {
  Rig *r = rig_new();
  Gen *g = calloc(1, sizeof *g);
  g->s = 0x9e3779b97f4a7c15ull ^ ((uint64_t)seed * 0xbf58476d1ce4e5b9ull) ^ (uint64_t)chaos;
  g->chaos = chaos;
  g->now = 1000;
  rig_at(r, g->now);
  rig_settle(r);
  for (int i = 0; i < steps; i++) gen_step(r, g);
  finish(r);

  Journal_View v;
  jv_load(r, &v);
  char why[1024];
  if (!jv_invariants(&v, why, sizeof why)) { fprintf(stderr, "seed %u chaos %d: %s\n", seed, chaos, why); t_fail++; }
  t_checks++;
  if (v.unterminated) { fprintf(stderr, "seed %u: unterminated journal\n", seed); t_fail++; }
  t_checks++;
  check_no_phantoms(g, &v, seed);
  tot_sessions[chaos] += v.sessions; tot_gaps[chaos] += v.gaps; tot_crashes[chaos] += r->crashes; tot_bad[chaos] += v.n_bad; tot_runs[chaos]++;

  if (!chaos) {
    SessionRec want[512];
    int n = oracle(g, want, 512);
    int got = 0, ok = 1;
    for (int i = 0; i < v.n_valid; i++) {
      if (!rec_is_session(v.line[i])) continue;
      char w[24];
      uint64_t ss = 0, es = 0;
      rec_str(v.line[i], "why", w, sizeof w); rec_num(v.line[i], "ss", &ss); rec_num(v.line[i], "es", &es);
      if (got >= n || want[got].ss != ss || strcmp(close_why_name(want[got].why), w) != 0 || (want[got].end_known && want[got].es != es)) ok = 0;
      got++;
    }
    if (got != n) ok = 0;
    if (!ok) { fprintf(stderr, "seed %u: watcher sessions differ from the oracle (%d vs %d)\n", seed, got, n); t_fail++; }
    t_checks++;
    FFile *j = ffs_find(&r->fs, AW_PATH_SESSIONS);
    (void)j;
    Fs fs = ffs_iface();
    PState st; uint32_t sq; int nx;
    if (state_load(&fs, &r->fs, &st, &sq, &nx) != ST_OK) { fprintf(stderr, "seed %u: no valid state at the end\n", seed); t_fail++; }
    t_checks++;
  }
  jv_free(&v);
  free(g);
  rig_del(r);
}

static void faults_crashes_and_clock_changes_keep_every_session_exactly_once(void) {
  for (uint32_t seed = 1; seed <= 600; seed++) run_seed(seed, 0, 90);
}

static void log_loss_and_damage_never_break_the_invariants(void) {
  for (uint32_t seed = 1; seed <= 600; seed++) run_seed(seed, 1, 90);
}

static void long_runs(void) {
  for (uint32_t seed = 5000; seed < 5030; seed++) { run_seed(seed, 0, 400); run_seed(seed, 1, 400); }
}

int main(void) {
  RUN(faults_crashes_and_clock_changes_keep_every_session_exactly_once);
  RUN(log_loss_and_damage_never_break_the_invariants);
  RUN(long_runs);
  for (int c = 0; c < 2; c++)
    printf("  %s: %ld runs, %ld sessions, %ld gaps, %ld crashes, %ld fenced garbage lines\n", c ? "chaos" : "no log loss", tot_runs[c],
           tot_sessions[c], tot_gaps[c], tot_crashes[c], tot_bad[c]);
  return t_done("prop");
}
