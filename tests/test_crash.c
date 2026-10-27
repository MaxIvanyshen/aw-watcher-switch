#include <errno.h>

#include "rig.h"
#include "state.h"
#include "t.h"

#define G1 0x0100000000001000ull
#define G2 0x0100000000002000ull

typedef void (*Scenario)(Rig *);

static void ev_at(Rig *r, int64_t t, RawEvent e) { rig_tick(r, t); rig_push(r, e); rig_settle(r); }
#define FOCUS(r, pid, t) ev_at(r, t, ev_app(pid, AE_IN_FOCUS, (uint64_t)(t)))
#define UNFOCUS(r, pid, t) ev_at(r, t, ev_app(pid, AE_OUT_OF_FOCUS, (uint64_t)(t)))

/* Several sessions of every kind, a multi-batch burst and an Initialize with a session open. */
static void scn_sessions(Rig *r) {
  rig_push(r, ev_app(G2, AE_IN_FOCUS, 50));
  rig_push(r, ev_account(60));
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000); UNFOCUS(r, G1, 1040);
  FOCUS(r, G1, 1060); ev_at(r, 1080, ev_power(1, 1080));
  FOCUS(r, G2, 1100); FOCUS(r, G1, 1120); UNFOCUS(r, G1, 1150);
  ev_at(r, 1150, ev_account(1150));
  ev_at(r, 1150, ev_app_pol(G2, AE_IN_FOCUS, POLICY_NONE, 1150));
  for (int i = 0; i < 36; i++) {                                  /* 72 events in one poll: three batches */
    rig_push(r, ev_app(G2, AE_IN_FOCUS, 1160 + (uint64_t)i * 2));
    rig_push(r, ev_app(G2, i % 2 ? AE_EXIT : AE_OUT_OF_FOCUS, 1161 + (uint64_t)i * 2));
  }
  rig_tick(r, 1240); rig_settle(r);
  FOCUS(r, G1, 1250);
  rig_tick(r, 1270);
  rig_at(r, 50000);
  rig_push(r, ev_init(50000)); rig_settle(r);
  FOCUS(r, G2, 50010); UNFOCUS(r, G2, 50030);
}

/* Epoch changes: steady source change, steady regression in the log, log expiry, log reset. */
static void scn_epochs(Rig *r) {
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000); UNFOCUS(r, G1, 1030);
  FOCUS(r, G2, 1040);
  rig_tick(r, 1060);
  r->env.src[0] = 5; r->env.steady = 20; rig_settle(r);             /* steady source replaced */
  FOCUS(r, G1, 30); UNFOCUS(r, G1, 50);
  FOCUS(r, G2, 60);
  ev_at(r, 70, ev_app(G1, AE_IN_FOCUS, 5));                        /* the log goes back in time */
  UNFOCUS(r, G1, 90);
  FOCUS(r, G1, 100);
  for (int i = 0; i < 12; i++) rig_push(r, ev_account(110));
  fp_expire(r->pdm, r->pdm->end - r->pdm->start - 3);              /* most of the unread log expires */
  rig_tick(r, 120);
  FOCUS(r, G2, 130);
  fp_reset(r->pdm);                                                /* then the whole log restarts */
  rig_push(r, ev_init(140));
  rig_tick(r, 140);
  FOCUS(r, G1, 150); UNFOCUS(r, G1, 170);
}

typedef struct { Journal_View v; uint32_t ops; } Ref;

static Ref reference(Scenario s) {
  Rig *r = rig_new();
  s(r);
  rig_settle(r);
  Ref ref;
  jv_load(r, &ref.v);
  ref.ops = r->fs.ops;
  char why[512];
  CHECK(jv_invariants(&ref.v, why, sizeof why));
  if (!jv_invariants(&ref.v, why, sizeof why)) fprintf(stderr, "reference: %s\n", why);
  CHECK_EQ(r->crashes, 0);
  rig_del(r);
  return ref;
}

static int state_loadable(Rig *r) {
  Fs fs = ffs_iface();
  PState st;
  uint32_t seq;
  int next;
  return state_load(&fs, &r->fs, &st, &seq, &next) == ST_OK;
}

typedef struct { int at2; CrashMode m2; } Second;

static void arm_second(Rig *r) {
  Second *s = r->user;
  if (s && s->at2 >= 0) {
    ffs_crash_plan(&r->fs, (int)r->fs.ops + s->at2, s->m2, &r->jb);
    s->at2 = -1;
  }
}

static int unreached;

static int run_one(Scenario s, const Ref *ref, int at, CrashMode m, Second *sec, const char *name) {
  Rig *r = rig_new();
  r->after_crash = arm_second;
  r->user = sec;
  ffs_crash_plan(&r->fs, at, m, &r->jb);
  s(r);
  for (int i = 0; i < 4; i++) rig_settle(r);
  if (r->crashes == 0) unreached++;
  Journal_View v;
  jv_load(r, &v);
  char why[1024] = "";
  int ok = 1;
  if (!jv_invariants(&v, why, sizeof why)) ok = 0;
  else if (!jv_equivalent(&ref->v, &v, why, sizeof why)) ok = 0;
  else if (v.unterminated) { snprintf(why, sizeof why, "journal ends in an unterminated line"); ok = 0; }
  else if (!state_loadable(r)) { snprintf(why, sizeof why, "no valid state slot at the end"); ok = 0; }
  if (!ok) fprintf(stderr, "%s: crash at op %d mode %d%s: %s\n", name, at, m, sec ? " (+second)" : "", why);
  jv_free(&v);
  rig_del(r);
  return ok;
}

static void sweep_single(Scenario s, const char *name) {
  Ref ref = reference(s);
  int runs = 0, bad = 0;
  for (int at = 0; at < (int)ref.ops; at++)
    for (int m = 0; m < CM_COUNT; m++) {
      runs++;
      if (!run_one(s, &ref, at, (CrashMode)m, NULL, name)) bad++;
    }
  printf("    %s: %d single-crash runs over %u mutating ops, %d failed, %d never crashed\n", name, runs, ref.ops, bad, unreached);
  CHECK(unreached < runs / 20);
  unreached = 0;
  CHECK_EQ(bad, 0);
  CHECK(ref.v.sessions >= 5);
  jv_free(&ref.v);
}

static void sweep_double(Scenario s, const char *name, int stride) {
  Ref ref = reference(s);
  int runs = 0, bad = 0;
  for (int at = 0; at < (int)ref.ops; at += stride)
    for (int m = 0; m < CM_COUNT; m++)
      for (int at2 = 0; at2 < 14; at2++)
        for (int m2 = 0; m2 < CM_COUNT; m2++) {
          Second sec = {at2, (CrashMode)m2};
          runs++;
          if (!run_one(s, &ref, at, (CrashMode)m, &sec, name)) bad++;
        }
  printf("    %s: %d double-crash runs, %d failed\n", name, runs, bad);
  CHECK_EQ(bad, 0);
  jv_free(&ref.v);
}

static void crash_sweep_sessions(void) { sweep_single(scn_sessions, "sessions"); }
static void crash_sweep_epochs(void) { sweep_single(scn_epochs, "epochs"); }
static void double_crash_sessions(void) { sweep_double(scn_sessions, "sessions", 2); }
static void double_crash_epochs(void) { sweep_double(scn_epochs, "epochs", 1); }

/* The same history with a transient append or state failure at every op instead of a crash. */
static void fault_sweep(Scenario s, const char *name) {
  Ref ref = reference(s);
  int bad = 0, runs = 0;
  for (int at = 0; at < (int)ref.ops; at++)
    for (int kind = 0; kind < 3; kind++) {
      Rig *r = rig_new();
      r->fs.fault_at = at; r->fs.fault_len = 1 + at % 3; r->fs.fault_kind = (FaultKind)kind; r->fs.fault_errno = EIO;
      s(r);
      for (int i = 0; i < 12; i++) rig_poll(r);
      Journal_View v;
      jv_load(r, &v);
      char why[1024] = "";
      runs++;
      if (!jv_invariants(&v, why, sizeof why) || !jv_equivalent(&ref.v, &v, why, sizeof why) || v.unterminated) {
        fprintf(stderr, "%s: fault at op %d kind %d: %s\n", name, at, kind, why);
        bad++;
      }
      jv_free(&v);
      rig_del(r);
    }
  printf("    %s: %d fault runs, %d failed\n", name, runs, bad);
  CHECK_EQ(bad, 0);
  jv_free(&ref.v);
}

static void faults_sessions(void) { fault_sweep(scn_sessions, "sessions"); }
static void faults_epochs(void) { fault_sweep(scn_epochs, "epochs"); }

int main(void) {
  RUN(crash_sweep_sessions);
  RUN(crash_sweep_epochs);
  RUN(faults_sessions);
  RUN(faults_epochs);
  RUN(double_crash_sessions);
  RUN(double_crash_epochs);
  return t_done("crash");
}
