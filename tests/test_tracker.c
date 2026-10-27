#include "fakes/fake_pdm.h"
#include "t.h"
#include "tracker.h"

#define G1 0x0100000000001000ull
#define G2 0x0100000000002000ull

typedef struct { Tracker t; TrackOut o; int recs; SessionRec last; } TT;

static void step(TT *x, RawEvent r) {
  Event e;
  decode_event(&r, &e);
  tracker_step(&x->t, &e, &x->o);
  if (x->o.has_rec) { x->recs++; x->last = x->o.rec; }
}

static void tt_init(TT *x) { memset(x, 0, sizeof *x); }

static void open_close_ok(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  CHECK_EQ(x.o.note, NOTE_OPEN); CHECK(!x.o.has_rec); CHECK(x.t.open);
  tracker_touch(&x.t, 1030); tracker_touch(&x.t, 1060); tracker_touch(&x.t, 1090);
  step(&x, ev_app(G1, AE_OUT_OF_FOCUS, 1100));
  CHECK_EQ(x.o.note, NOTE_CLOSE);
  CHECK(x.o.has_rec);
  CHECK(!x.t.open);
  CHECK(x.last.pid == G1);
  CHECK_EQ(x.last.ss, 1000); CHECK_EQ(x.last.es, 1100); CHECK_EQ(x.last.dur, 100);
  CHECK(x.last.dur_ok); CHECK(x.last.end_known);
  CHECK_EQ(x.last.why, WHY_OUT_OF_FOCUS); CHECK_EQ(x.last.sem, SEM_OK);
}

static void close_variants(void) {
  uint8_t outs[] = {AE_OUT_OF_FOCUS, AE_OUT_OF_FOCUS4};
  uint8_t exits[] = {AE_EXIT, AE_EXIT5, AE_EXIT6};
  for (unsigned i = 0; i < sizeof outs; i++) {
    TT x; tt_init(&x);
    step(&x, ev_app(G1, AE_IN_FOCUS, 1000)); step(&x, ev_app(G1, outs[i], 1010));
    CHECK_EQ(x.last.why, WHY_OUT_OF_FOCUS);
  }
  for (unsigned i = 0; i < sizeof exits; i++) {
    TT x; tt_init(&x);
    step(&x, ev_app(G1, AE_IN_FOCUS, 1000)); step(&x, ev_app(G1, exits[i], 1010));
    CHECK_EQ(x.last.why, WHY_EXIT); CHECK_EQ(x.last.sem, SEM_OK);
  }
}

static void ignores(void) {
  TT x; tt_init(&x);
  step(&x, ev_raw(PT_APPLET, G1, 0x02, 0, AE_IN_FOCUS, 0, 1, 1, 1000));
  CHECK_EQ(x.o.note, NOTE_IGN_APPLET_ID); CHECK(!x.t.open);
  step(&x, ev_app_pol(G1, AE_IN_FOCUS, POLICY_NONE, 1001));
  CHECK_EQ(x.o.note, NOTE_IGN_POLICY); CHECK(!x.t.open);
  step(&x, ev_app(G1, AE_LAUNCH, 1002));
  CHECK_EQ(x.o.note, NOTE_IGN_LAUNCH);
  step(&x, ev_app(G1, AE_OUT_OF_FOCUS, 1003));
  CHECK_EQ(x.o.note, NOTE_IGN_NOMATCH);
  step(&x, ev_account(1004));
  CHECK_EQ(x.o.note, NOTE_IGN_ACCOUNT);
  step(&x, ev_raw(PT_OPMODE, 0, 0, 0, 0, 1, 1, 1, 1005));
  CHECK_EQ(x.o.note, NOTE_IGN_OPMODE);
  step(&x, ev_power(1, 1006));
  CHECK_EQ(x.o.note, NOTE_IGN_POWER);
  step(&x, ev_init(1007));
  CHECK_EQ(x.o.note, NOTE_IGN_INIT);
  step(&x, ev_raw(9, 0, 0, 0, 0, 0, 1, 1, 1008));
  CHECK_EQ(x.o.note, NOTE_IGN_UNKNOWN);
  step(&x, ev_app(G1, 9, 1009));
  CHECK_EQ(x.o.note, NOTE_IGN_UNKNOWN);
  CHECK_EQ(x.recs, 0);
}

static void none_policy_never_closes(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  step(&x, ev_app_pol(G1, AE_OUT_OF_FOCUS, POLICY_NONE, 1010));
  CHECK_EQ(x.o.note, NOTE_IGN_POLICY);
  CHECK(x.t.open);
}

static void policy_is_carried(void) {
  TT x; tt_init(&x);
  step(&x, ev_app_pol(G1, AE_IN_FOCUS, POLICY_LOG_ONLY, 1000));
  step(&x, ev_app_pol(G1, AE_OUT_OF_FOCUS, POLICY_LOG_ONLY, 1010));
  CHECK_EQ(x.last.pol, POLICY_LOG_ONLY);
}

static void duplicate_focus_keeps_start(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  step(&x, ev_app(G1, AE_IN_FOCUS, 1005));
  CHECK_EQ(x.o.note, NOTE_DUP_FOCUS); CHECK_EQ(x.recs, 0);
  step(&x, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
  CHECK_EQ(x.last.ss, 1000); CHECK_EQ(x.last.dur, 10);
}

static void superseded_is_never_ok(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  step(&x, ev_app(G2, AE_IN_FOCUS, 1020));
  CHECK_EQ(x.o.note, NOTE_SUPERSEDE); CHECK_EQ(x.recs, 1);
  CHECK(x.last.pid == G1); CHECK_EQ(x.last.why, WHY_SUPERSEDED); CHECK_EQ(x.last.sem, SEM_UNC);
  CHECK_EQ(x.last.es, 1020);
  CHECK(x.t.open && x.t.pid == G2 && x.t.ss == 1020);
}

static void other_program_out_of_focus_is_ignored(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  step(&x, ev_app(G2, AE_OUT_OF_FOCUS, 1010));
  CHECK_EQ(x.o.note, NOTE_IGN_NOMATCH); CHECK(x.t.open);
}

static void power_policy_closes_as_experimental(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  step(&x, ev_power(3, 1020));
  CHECK_EQ(x.recs, 1); CHECK_EQ(x.last.why, WHY_POWER); CHECK_EQ(x.last.sem, SEM_EXP);
  CHECK_EQ(x.last.es, 1020); CHECK(x.last.end_known);
  step(&x, ev_power(3, 1030));
  CHECK_EQ(x.recs, 1);
}

/* R8: Initialize starts a fresh log. The time since the session was last seen is unknown. */
static void initialize_closes_at_last_alive_not_at_event(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  tracker_touch(&x.t, 1020);
  step(&x, ev_init(90000));   /* console was off for a day */
  CHECK_EQ(x.recs, 1);
  CHECK_EQ(x.last.why, WHY_INIT); CHECK_EQ(x.last.sem, SEM_UNC);
  CHECK(!x.last.end_known);
  CHECK_EQ(x.last.es, 1020); CHECK_EQ(x.last.dur, 20);
}

static void alive_does_not_jump_over_a_stall(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  tracker_touch(&x.t, 1020);
  tracker_touch(&x.t, 1020 + ALIVE_GAP_MAX + 1);    /* suspended or off */
  CHECK_EQ(x.t.alive, 1020);
  tracker_touch(&x.t, 1020 + ALIVE_GAP_MAX + 5);    /* stays frozen, even with later short steps */
  CHECK_EQ(x.t.alive, 1020);
  tracker_touch(&x.t, 1020 + ALIVE_GAP_MAX);
  CHECK_EQ(x.t.alive, 1020 + ALIVE_GAP_MAX);
}

static void closing_after_a_stall_is_uncertain(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  tracker_touch(&x.t, 1010);
  step(&x, ev_app(G1, AE_OUT_OF_FOCUS, 5000));      /* real event, but the watcher could not vouch for the time in between */
  CHECK_EQ(x.last.sem, SEM_UNC);
  CHECK(x.last.end_known);
  CHECK_EQ(x.last.dur, 4000);
}

static void closing_within_step_is_ok(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  tracker_touch(&x.t, 1010);
  step(&x, ev_app(G1, AE_OUT_OF_FOCUS, 1010 + ALIVE_GAP_MAX));
  CHECK_EQ(x.last.sem, SEM_OK);
}

static void steady_regression_resets(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 5000));
  tracker_touch(&x.t, 5010);
  step(&x, ev_account(100));    /* clock went back */
  CHECK(x.o.bump);
  CHECK_EQ(x.recs, 1); CHECK_EQ(x.last.why, WHY_CLOCK_RESET); CHECK_EQ(x.last.sem, SEM_UNC);
  CHECK_EQ(x.last.es, 5010);
  CHECK(!x.t.open);
  step(&x, ev_app(G1, AE_IN_FOCUS, 120));
  CHECK(!x.o.bump); CHECK(x.t.open);
}

static void regression_event_is_applied_after_reset(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 5000));
  step(&x, ev_app(G2, AE_IN_FOCUS, 100));
  CHECK(x.o.bump); CHECK_EQ(x.recs, 1);
  CHECK(x.last.pid == G1); CHECK_EQ(x.last.why, WHY_CLOCK_RESET);
  CHECK(x.t.open && x.t.pid == G2);
}

static void zero_steady_gives_null_duration(void) {
  TT x; tt_init(&x);
  step(&x, ev_app(G1, AE_IN_FOCUS, 0));
  step(&x, ev_app(G1, AE_OUT_OF_FOCUS, 0));
  CHECK_EQ(x.recs, 1); CHECK(!x.last.dur_ok); CHECK_EQ(x.last.sem, SEM_UNC);
  TT y; tt_init(&y);
  step(&y, ev_app(G1, AE_IN_FOCUS, 1000));
  step(&y, ev_raw(PT_ACCOUNT, 0, 0, 0, 0, 0, 1, 1, 0));   /* zero steady must not look like a regression */
  CHECK(!y.o.bump); CHECK(y.t.open);
}

static void user_and_network_clocks_never_affect_duration(void) {
  TT x; tt_init(&x);
  step(&x, ev_raw(PT_APPLET, G1, 1, 0, AE_IN_FOCUS, 0, 2000000000ull, 0, 1000));
  tracker_touch(&x.t, 1030);
  step(&x, ev_raw(PT_APPLET, G1, 1, 0, AE_OUT_OF_FOCUS, 0, 100, 5, 1060));   /* user clock set back by years, network zero */
  CHECK_EQ(x.recs, 1); CHECK_EQ(x.last.dur, 60); CHECK_EQ(x.last.sem, SEM_OK);
  CHECK_EQ(x.last.su, 2000000000ull); CHECK_EQ(x.last.sn, 0); CHECK_EQ(x.last.eu, 100); CHECK_EQ(x.last.en, 5);
  CHECK(!x.o.bump);
}

static void close_unc_reports_state(void) {
  TT x; tt_init(&x);
  SessionRec r;
  CHECK_EQ(tracker_close_unc(&x.t, WHY_LOST, &r), 0);
  step(&x, ev_app(G1, AE_IN_FOCUS, 1000));
  tracker_touch(&x.t, 1007);
  CHECK_EQ(tracker_close_unc(&x.t, WHY_LOST, &r), 1);
  CHECK_EQ(r.why, WHY_LOST); CHECK_EQ(r.sem, SEM_UNC); CHECK_EQ(r.dur, 7); CHECK(!r.end_known);
  CHECK(!x.t.open);
}

static void multi_session_batch(void) {
  TT x; tt_init(&x);
  int expect = 0;
  for (int i = 0; i < 20; i++) {
    uint64_t t = 1000 + (uint64_t)i * 100;
    step(&x, ev_app(G1, AE_IN_FOCUS, t));
    step(&x, ev_app(G1, i % 2 ? AE_EXIT : AE_OUT_OF_FOCUS, t + 50));
    expect++;
    CHECK_EQ(x.last.dur, 50);
  }
  CHECK_EQ(x.recs, expect);
}

static void names(void) {
  CHECK_STR(close_why_name(WHY_INIT), "init");
  CHECK_STR(sem_name(SEM_UNC), "unc");
  CHECK_STR(note_name(NOTE_DUP_FOCUS), "dup_focus");
  CHECK_STR(close_why_name(99), "?");
}

int main(void) {
  RUN(open_close_ok);
  RUN(close_variants);
  RUN(ignores);
  RUN(none_policy_never_closes);
  RUN(policy_is_carried);
  RUN(duplicate_focus_keeps_start);
  RUN(superseded_is_never_ok);
  RUN(other_program_out_of_focus_is_ignored);
  RUN(power_policy_closes_as_experimental);
  RUN(initialize_closes_at_last_alive_not_at_event);
  RUN(alive_does_not_jump_over_a_stall);
  RUN(closing_after_a_stall_is_uncertain);
  RUN(closing_within_step_is_ok);
  RUN(steady_regression_resets);
  RUN(regression_event_is_applied_after_reset);
  RUN(zero_steady_gives_null_duration);
  RUN(user_and_network_clocks_never_affect_duration);
  RUN(close_unc_reports_state);
  RUN(multi_session_batch);
  RUN(names);
  return t_done("tracker");
}
