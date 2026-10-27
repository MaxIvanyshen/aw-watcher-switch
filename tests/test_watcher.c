#include <errno.h>

#include "rig.h"
#include "t.h"

#define G1 0x0100000000001000ull
#define G2 0x0100000000002000ull

static void ev_at(Rig *r, int64_t t, RawEvent e) { rig_tick(r, t); rig_push(r, e); rig_settle(r); }
#define FOCUS(r, pid, t) ev_at(r, t, ev_app(pid, AE_IN_FOCUS, (uint64_t)(t)))
#define UNFOCUS(r, pid, t) ev_at(r, t, ev_app(pid, AE_OUT_OF_FOCUS, (uint64_t)(t)))

static uint64_t num(const char *line, const char *f) { uint64_t v = ~0ull; rec_num(line, f, &v); return v; }
static int is(const char *line, const char *f, const char *want) {
  char b[32];
  return rec_str(line, f, b, sizeof b) && strcmp(b, want) == 0;
}

static void first_run_starts_at_tail(void) {
  Rig *r = rig_new();
  for (int i = 0; i < 5; i++) rig_push(r, ev_app(i % 2 ? G2 : G1, i % 2 ? AE_OUT_OF_FOCUS : AE_IN_FOCUS, 100 + (uint64_t)i));
  rig_at(r, 1000);
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.n_valid, 1);
  CHECK(!rec_is_session(v.line[0]));
  CHECK(is(v.line[0], "why", "first_run"));
  CHECK_EQ(num(v.line[0], "ep"), 1); CHECK_EQ(num(v.line[0], "ev"), 5);
  CHECK_EQ(num(v.line[0], "from"), 5); CHECK_EQ(num(v.line[0], "to"), 5);
  jv_free(&v);

  FOCUS(r, G1, 1000);
  UNFOCUS(r, G1, 1030);
  jv_load(r, &v);
  CHECK_EQ(v.n_valid, 2);
  CHECK(rec_is_session(v.line[1]));
  CHECK_EQ(num(v.line[1], "dur"), 30); CHECK_EQ(num(v.line[1], "ss"), 1000); CHECK_EQ(num(v.line[1], "es"), 1030);
  CHECK(is(v.line[1], "why", "out_of_focus")); CHECK(is(v.line[1], "sem", "ok"));
  CHECK_EQ(num(v.line[1], "ev"), 6); CHECK_EQ(num(v.line[1], "sub"), 1);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  rig_del(r);
}

static void record_is_written_once_however_often_we_poll_or_restart(void) {
  Rig *a = rig_new(), *b = rig_new();
  for (int round = 0; round < 2; round++) {
    Rig *r = round ? b : a;
    rig_at(r, 1000); rig_settle(r);
    for (int s = 0; s < 6; s++) {
      int64_t t = 1000 + s * 100;
      ev_at(r, t + 10, ev_app(G1, AE_IN_FOCUS, (uint64_t)t + 10));
      if (round) rig_boot(r);
      ev_at(r, t + 40, ev_app(G1, AE_OUT_OF_FOCUS, (uint64_t)t + 40));
      if (round) { rig_boot(r); rig_poll(r); rig_boot(r); }
      rig_poll(r); rig_poll(r);
    }
  }
  Journal_View va, vb;
  jv_load(a, &va); jv_load(b, &vb);
  CHECK_EQ(va.sessions, 6); CHECK_EQ(vb.sessions, 6);
  CHECK_EQ(va.n_valid, vb.n_valid);
  for (int i = 0; i < va.n_valid && i < vb.n_valid; i++) CHECK_STR(va.line[i], vb.line[i]);
  CHECK(jv_keys_increasing(&vb));
  jv_free(&va); jv_free(&vb);
  rig_del(a); rig_del(b);
}

static void end_index_inclusive_or_exclusive_both_work(void) {
  for (int incl = 0; incl < 2; incl++) {
    Rig *r = rig_new();
    r->pdm->end_inclusive = incl;
    rig_push(r, ev_app(G2, AE_IN_FOCUS, 50));
    rig_push(r, ev_app(G2, AE_OUT_OF_FOCUS, 60));
    rig_at(r, 1000); rig_settle(r);
    FOCUS(r, G1, 1000);
    UNFOCUS(r, G1, 1020);
    FOCUS(r, G1, 1030);
    UNFOCUS(r, G1, 1050);
    Journal_View v;
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 2);
    CHECK_EQ(num(v.line[0], "ev"), 2);        /* first run cursor is the true tail either way */
    CHECK_EQ(num(v.line[1], "ev"), 3);
    CHECK_EQ(num(v.line[2], "ev"), 5);
    jv_free(&v);
    rig_del(r);
  }
}

static void past_end_query_error_counts_as_end_of_log(void) {
  Rig *r = rig_new();
  r->pdm->past_end_err = 1;
  rig_push(r, ev_app(G2, AE_IN_FOCUS, 50));
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  UNFOCUS(r, G1, 1020);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.gaps, 1); CHECK_EQ(num(v.line[0], "ev"), 1);
  CHECK_EQ(v.sessions, 1);
  jv_free(&v);
  rig_del(r);
}

static void past_end_errors_preserve_idle_heartbeat_and_empty_reset(void) {
  for (int incl = 0; incl < 2; incl++) {
    Rig *r = rig_new();
    r->pdm->end_inclusive = incl;
    r->pdm->past_end_err = 1;
    rig_settle(r);
    FOCUS(r, G1, 1000);
    rig_tick(r, 1100); /* idle EOF must still touch and persist liveness */
    for (int i = 0; i < 8; i++) rig_poll(r);
    CHECK_EQ(r->last_sleep, POLL_MS_EXPECT);
    UNFOCUS(r, G1, 1110);
    Journal_View v;
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 1);
    CHECK(is(v.line[jv_find(&v, 0, "\"t\":\"session\"")], "sem", "ok"));
    jv_free(&v);
    rig_del(r);

    /* One entry -> empty, cursor 1/end 0: formerly stuck on the anchor error. */
    r = rig_new();
    r->pdm->end_inclusive = incl;
    rig_settle(r);
    FOCUS(r, G1, 1000);
    r->pdm->past_end_err = 1;
    fp_reset(r->pdm);
    for (int i = 0; i < 8; i++) rig_poll(r);
    CHECK_EQ(r->last_sleep, POLL_MS_EXPECT);
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 1);
    CHECK_EQ(jv_count(&v, "log_reset"), 1);
    CHECK_EQ(jv_count(&v, "\"why\":\"lost\""), 1);
    CHECK(jv_keys_increasing(&v));
    CHECK(is(v.line[jv_find(&v, 0, "\"t\":\"session\"")], "sem", "unc"));
    jv_free(&v);
    rig_del(r);
  }
}

static void query_errors_below_tail_still_back_off(void) {
  for (int incl = 0; incl < 2; incl++) {
    Rig *r = rig_new();
    r->pdm->end_inclusive = incl;
    r->pdm->past_end_err = 1;
    rig_settle(r);
    FOCUS(r, G1, 1000);
    for (int i = 0; i < 5; i++) rig_push(r, ev_account(1005));
    rig_push(r, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
    r->pdm->query_err = EIO;
    r->pdm->query_err_n = -1;
    r->pdm->query_err_from = 1;
    r->pdm->query_err_limit = 2; /* tail probes succeed; the normal batch read fails below tail */
    rig_poll(r); rig_poll(r);
    CHECK_EQ(r->last_sleep, 10000);
    Journal_View v;
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 0);
    CHECK_EQ(v.gaps, 1);
    jv_free(&v);
    r->pdm->query_err = 0;
    rig_settle(r);
    CHECK_EQ(r->last_sleep, POLL_MS_EXPECT);
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 1);
    jv_free(&v);
    rig_del(r);
  }
}

static void error_on_a_single_existing_entry_is_not_empty_log(void) {
  for (int incl = 0; incl < 2; incl++) {
    Rig *r = rig_new();
    r->pdm->end_inclusive = incl;
    rig_settle(r);
    FOCUS(r, G1, 1000);
    rig_boot(r); /* also require the saved anchor to be checked */
    r->pdm->query_err = EIO;
    r->pdm->query_err_n = -1;
    r->pdm->query_err_limit = 1;
    rig_poll(r); rig_poll(r);
    CHECK_EQ(r->last_sleep, 10000);
    Journal_View v;
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 0);
    CHECK_EQ(jv_count(&v, "log_reset"), 0);
    jv_free(&v);
    r->pdm->query_err = 0;
    UNFOCUS(r, G1, 1020);
    CHECK_EQ(r->last_sleep, POLL_MS_EXPECT);
    jv_load(r, &v);
    CHECK_EQ(v.sessions, 1);
    jv_free(&v);
    rig_del(r);
  }
}

static void ignored_events_make_no_records(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_account(1000));
  ev_at(r, 1000, ev_raw(PT_OPMODE, 0, 0, 0, 0, 1, 1, 1, 1000));
  ev_at(r, 1000, ev_raw(PT_APPLET, G1, 0x02, 0, AE_IN_FOCUS, 0, 1, 1, 1000));
  ev_at(r, 1000, ev_app_pol(G1, AE_IN_FOCUS, POLICY_NONE, 1000));
  ev_at(r, 1001, ev_app(G1, AE_LAUNCH, 1001));
  ev_at(r, 1002, ev_power(2, 1002));
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 0); CHECK_EQ(v.gaps, 1);
  jv_free(&v);
  rig_del(r);
}

static void big_backlog_is_bounded_per_poll(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  for (int i = 0; i < 3000; i++) rig_push(r, ev_account(1000));
  rig_push(r, ev_app(G1, AE_IN_FOCUS, 1000));
  uint32_t q0 = r->pdm->query_calls;
  rig_poll(r);
  CHECK_EQ((long long)(r->pdm->query_calls - q0), 66);   /* two tail probes, 64 batches max, then yield */
  CHECK_EQ(r->last_sleep, BUSY_MS_EXPECT);
  rig_settle(r);
  CHECK_EQ(r->last_sleep, POLL_MS_EXPECT);
  ev_at(r, 1020, ev_app(G1, AE_OUT_OF_FOCUS, 1020));
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  jv_free(&v);
  rig_del(r);
}

static void user_and_network_clock_changes_never_touch_durations(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  ev_at(r, 1000, ev_raw(PT_APPLET, G1, 1, 0, AE_IN_FOCUS, 0, 1900000000ull, 0, 1000));
  ev_at(r, 1040, ev_raw(PT_APPLET, G1, 1, 0, AE_OUT_OF_FOCUS, 0, 946684800ull, 0, 1040));   /* user clock set to 2000, network off */
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  const char *l = v.line[1];
  CHECK_EQ(num(l, "dur"), 40);
  CHECK_EQ(num(l, "su"), 1900000000ull); CHECK_EQ(num(l, "eu"), 946684800ull);
  CHECK_EQ(num(l, "sn"), 0); CHECK_EQ(num(l, "en"), 0);
  CHECK(is(l, "sem", "ok"));
  CHECK_EQ(v.gaps, 1);
  jv_free(&v);
  rig_del(r);
}

static void steady_source_change_starts_new_epoch(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  rig_tick(r, 1020);
  r->env.src[0] = 9;                   /* steady clock source replaced */
  r->env.steady = 50;
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  int i = jv_find(&v, 0, "\"t\":\"session\"");
  CHECK(is(v.line[i], "why", "clock_reset")); CHECK(is(v.line[i], "sem", "unc"));
  CHECK_EQ(num(v.line[i], "es"), 1020); CHECK_EQ(num(v.line[i], "dur"), 20);
  CHECK(rec_is_null(v.line[i], "eu"));
  int g = jv_find(&v, 1, "steady_reset");
  CHECK(g > i); CHECK_EQ(num(v.line[g], "ep"), 2);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  /* new epoch keeps working on the new clock */
  FOCUS(r, G2, 60);
  UNFOCUS(r, G2, 80);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(num(v.line[v.n_valid - 1], "ep"), 2); CHECK_EQ(num(v.line[v.n_valid - 1], "dur"), 20);
  jv_free(&v);
  rig_del(r);
}

static void steady_regression_in_the_log_starts_new_epoch(void) {
  Rig *r = rig_new();
  rig_at(r, 5000); rig_settle(r);
  FOCUS(r, G1, 5000);
  rig_tick(r, 5020);
  ev_at(r, 5020, ev_app(G2, AE_IN_FOCUS, 100));       /* event stamped far in the past */
  UNFOCUS(r, G2, 5040);                                /* regressed relative to nothing: 100 -> 5040 is forward again */
  Journal_View v;
  jv_load(r, &v);
  int s0 = jv_find(&v, 0, "\"why\":\"clock_reset\"");
  CHECK(s0 >= 0);
  CHECK(is(v.line[s0], "sem", "unc"));
  CHECK_EQ(num(v.line[s0], "ep"), 1);
  int g = jv_find(&v, 0, "steady_reset");
  CHECK(g > s0); CHECK_EQ(num(v.line[g], "ep"), 2); CHECK_EQ(num(v.line[g], "ev"), 2); CHECK_EQ(num(v.line[g], "from"), 1);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  rig_del(r);
}

/* R8: the console powered off with a game open. Next boot starts a fresh log with Initialize. */
static void initialize_after_a_long_off_period_does_not_inflate_the_session(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  rig_tick(r, 1100);                                    /* watcher sees it alive until 1100, then power is cut */
  rig_boot(r);
  rig_at(r, 80000);                                     /* a day later */
  rig_push(r, ev_init(80000));
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  const char *l = v.line[jv_find(&v, 0, "\"t\":\"session\"")];
  CHECK(is(l, "why", "init")); CHECK(is(l, "sem", "unc"));
  CHECK(rec_is_null(l, "eu"));
  CHECK(num(l, "es") >= 1100 - 60 && num(l, "es") <= 1100);       /* last persisted heartbeat, not the event time */
  CHECK(num(l, "dur") <= 100);
  jv_free(&v);
  rig_del(r);
}

static void restart_after_a_long_gap_never_labels_the_session_accurate(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  rig_tick(r, 1100);
  rig_boot(r);
  rig_at(r, 90000);
  rig_settle(r);
  ev_at(r, 90010, ev_app(G1, AE_OUT_OF_FOCUS, 90010));    /* same game reported out of focus after the long gap */
  Journal_View v;
  jv_load(r, &v);
  const char *l = v.line[jv_find(&v, 0, "\"t\":\"session\"")];
  CHECK(is(l, "sem", "unc"));
  CHECK_EQ(num(l, "dur"), 89010);                          /* real timestamps, but labelled uncertain */
  jv_free(&v);
  rig_del(r);
}

static void suspend_inside_a_session_is_labelled_uncertain(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  rig_tick(r, 1030);
  rig_at(r, 4000);                                         /* the watcher was suspended, no events logged */
  rig_settle(r);
  ev_at(r, 4010, ev_app(G1, AE_OUT_OF_FOCUS, 4010));
  Journal_View v;
  jv_load(r, &v);
  CHECK(is(v.line[jv_find(&v, 0, "\"t\":\"session\"")], "sem", "unc"));
  jv_free(&v);
  rig_del(r);
}

static void power_event_closes_as_experimental(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  ev_at(r, 1050, ev_power(1, 1050));
  Journal_View v;
  jv_load(r, &v);
  const char *l = v.line[jv_find(&v, 0, "\"t\":\"session\"")];
  CHECK(is(l, "why", "power")); CHECK(is(l, "sem", "exp")); CHECK_EQ(num(l, "dur"), 50);
  jv_free(&v);
  rig_del(r);
}

static void superseded_session_is_uncertain(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  FOCUS(r, G2, 1040);
  UNFOCUS(r, G2, 1070);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  int i = jv_find(&v, 0, "superseded");
  CHECK(i >= 0 && is(v.line[i], "sem", "unc"));
  CHECK_EQ(num(v.line[i], "es"), 1040);
  jv_free(&v);
  rig_del(r);
}

static void restart_mid_session_keeps_real_timestamps_but_is_labelled_uncertain(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  rig_tick(r, 1170);
  rig_boot(r);                                             /* process restart mid session */
  rig_tick(r, 1180);
  ev_at(r, 1190, ev_app(G1, AE_OUT_OF_FOCUS, 1190));
  Journal_View v;
  jv_load(r, &v);
  const char *l = v.line[jv_find(&v, 0, "\"t\":\"session\"")];
  CHECK_EQ(num(l, "dur"), 190);
  CHECK(is(l, "sem", "unc"));
  jv_free(&v);
  rig_del(r);
}

static void log_expiry_closes_the_session_and_marks_the_gap(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);                 /* ev 0 */
  rig_tick(r, 1050);
  rig_boot(r);                        /* watcher down while the log moves on */
  for (int i = 0; i < 10; i++) rig_push(r, ev_account(1060));
  fp_expire(r->pdm, 8);
  rig_at(r, 1060);
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  int s = jv_find(&v, 0, "\"t\":\"session\"");
  CHECK(s >= 0);
  CHECK(is(v.line[s], "why", "lost")); CHECK(is(v.line[s], "sem", "unc"));
  CHECK_EQ(num(v.line[s], "es"), 1000);                    /* last persisted sighting, the restart lost 50 s of RAM */
  int g = jv_find(&v, 0, "log_dropped");
  CHECK(g > s);
  CHECK_EQ(num(v.line[g], "ep"), 2); CHECK_EQ(num(v.line[g], "from"), 1); CHECK_EQ(num(v.line[g], "to"), 8);
  CHECK_EQ(num(v.line[g], "ev"), 8);
  jv_free(&v);
  FOCUS(r, G2, 1070);
  UNFOCUS(r, G2, 1090);
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 2);
  CHECK_EQ(num(v.line[v.n_valid - 1], "ep"), 2);
  CHECK(jv_keys_increasing(&v));
  jv_free(&v);
  rig_del(r);
}

static void log_reset_with_fewer_entries_is_detected(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  for (int i = 0; i < 6; i++) rig_push(r, ev_account(1000));
  FOCUS(r, G1, 1000);
  rig_tick(r, 1020);
  fp_reset(r->pdm);
  rig_push(r, ev_init(1030));
  rig_tick(r, 1030);
  Journal_View v;
  jv_load(r, &v);
  CHECK(jv_find(&v, 0, "log_reset") > 0);
  int s = jv_find(&v, 0, "\"why\":\"lost\"");
  CHECK(s >= 0 && is(v.line[s], "sem", "unc") && num(v.line[s], "es") == 1020);
  jv_free(&v);
  FOCUS(r, G2, 1040);
  UNFOCUS(r, G2, 1060);
  jv_load(r, &v);
  CHECK_EQ(num(v.line[v.n_valid - 1], "ep"), 2);
  CHECK_EQ(num(v.line[v.n_valid - 1], "dur"), 20);
  jv_free(&v);
  rig_del(r);
}

static void log_reset_refilled_past_the_cursor_is_caught_by_the_anchor_on_restart(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);                         /* ev 0 */
  rig_tick(r, 1010);
  rig_boot(r);
  fp_reset(r->pdm);
  for (int i = 0; i < 5; i++) rig_push(r, ev_app(G2, i % 2 ? AE_OUT_OF_FOCUS : AE_IN_FOCUS, 1200 + (uint64_t)i));
  rig_at(r, 1210);
  rig_settle(r);
  Journal_View v;
  jv_load(r, &v);
  CHECK(jv_find(&v, 0, "log_reset") > 0);
  CHECK(jv_find(&v, 0, "\"why\":\"lost\"") > 0);
  CHECK_EQ(v.sessions, 3);        /* lost session, then the two sessions of the new log, read from its start */
  jv_free(&v);
  rig_del(r);
}

static void log_that_shrinks_and_regrows_is_caught_by_range_movement(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  for (int i = 0; i < 4; i++) rig_push(r, ev_account(1000 + (uint64_t)i));
  rig_tick(r, 1010);
  fp_reset(r->pdm);
  rig_tick(r, 1020);                                  /* poll sees end < last end */
  rig_push(r, ev_app(G2, AE_IN_FOCUS, 1021));
  for (int i = 0; i < 6; i++) rig_push(r, ev_account(1021));
  rig_tick(r, 1030);
  Journal_View v;
  jv_load(r, &v);
  CHECK(jv_find(&v, 0, "log_reset") > 0);
  jv_free(&v);
  rig_del(r);
}

static void garbage_range_changes_nothing_and_backs_off(void) {
  for (int g = 1; g <= 4; g++) {
    Rig *r = rig_new();
    rig_at(r, 1000); rig_settle(r);
    FOCUS(r, G1, 1000);
    Journal_View v0, v1;
    jv_load(r, &v0);
    uint8_t a0[200], b0[200], a1[200], b1[200];
    size_t an0, bn0, an1, bn1;
    snapshot_state(r, a0, &an0, b0, &bn0);
    r->pdm->range_garbage = g;
    rig_tick(r, 1050);
    CHECK_EQ(r->last_sleep >= 5000, 1);
    jv_load(r, &v1);
    CHECK_EQ(v1.n_valid, v0.n_valid);
    snapshot_state(r, a1, &an1, b1, &bn1);
    CHECK(an0 == an1 && bn0 == bn1 && memcmp(a0, a1, an0) == 0 && memcmp(b0, b1, bn0) == 0);
    r->pdm->range_garbage = 0;
    UNFOCUS(r, G1, 1060);
    jv_load(r, &v1);
    CHECK_EQ(v1.sessions, 1);
    CHECK_EQ(jv_find(&v1, 0, "log_"), -1);
    jv_free(&v0); jv_free(&v1);
    rig_del(r);
  }
}

static void pdm_errors_back_off_without_losing_events(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  FOCUS(r, G1, 1000);
  r->pdm->range_err = EIO;
  rig_push(r, ev_app(G1, AE_OUT_OF_FOCUS, 1010));
  rig_at(r, 1010);
  rig_poll(r);
  CHECK_EQ(r->last_sleep, 5000);
  rig_poll(r);
  CHECK_EQ(r->last_sleep, 10000);
  r->pdm->range_err = 0;
  r->pdm->query_err = EIO; r->pdm->query_err_n = -1;
  rig_poll(r);
  CHECK(r->last_sleep > 10000);
  r->pdm->query_err = 0;
  rig_settle(r);
  CHECK_EQ(r->last_sleep, 5000);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  jv_free(&v);
  rig_del(r);
}

static void backoff_is_capped(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  r->pdm->range_err = EIO;
  for (int i = 0; i < 20; i++) rig_poll(r);
  CHECK_EQ(r->last_sleep, 300000);
  rig_del(r);
}

static void steady_clock_failure_does_not_stop_recording(void) {
  Rig *r = rig_new();
  rig_at(r, 1000); rig_settle(r);
  r->env.steady_err = EIO;
  FOCUS(r, G1, 1000);
  UNFOCUS(r, G1, 1020);
  Journal_View v;
  jv_load(r, &v);
  CHECK_EQ(v.sessions, 1);
  jv_free(&v);
  rig_del(r);
}

static void init_rejects_missing_callbacks(void) {
  Rig *r = rig_new();
  Watcher *w = (Watcher *)&r->store;
  PdmSrc bad = {NULL, NULL};
  CHECK(watcher_init(w, bad, r->pdm, ffs_iface(), &r->fs, fenv_iface(), &r->env) != 0);
  CHECK(watcher_init(NULL, fp_iface(), r->pdm, ffs_iface(), &r->fs, fenv_iface(), &r->env) != 0);
  CHECK(watcher_poll(NULL, NULL) != 0);
  CHECK(watcher_size() <= sizeof(WatcherStorage));
  rig_del(r);
}

int main(void) {
  RUN(first_run_starts_at_tail);
  RUN(record_is_written_once_however_often_we_poll_or_restart);
  RUN(end_index_inclusive_or_exclusive_both_work);
  RUN(past_end_query_error_counts_as_end_of_log);
  RUN(past_end_errors_preserve_idle_heartbeat_and_empty_reset);
  RUN(query_errors_below_tail_still_back_off);
  RUN(error_on_a_single_existing_entry_is_not_empty_log);
  RUN(ignored_events_make_no_records);
  RUN(big_backlog_is_bounded_per_poll);
  RUN(user_and_network_clock_changes_never_touch_durations);
  RUN(steady_source_change_starts_new_epoch);
  RUN(steady_regression_in_the_log_starts_new_epoch);
  RUN(initialize_after_a_long_off_period_does_not_inflate_the_session);
  RUN(restart_after_a_long_gap_never_labels_the_session_accurate);
  RUN(suspend_inside_a_session_is_labelled_uncertain);
  RUN(power_event_closes_as_experimental);
  RUN(superseded_session_is_uncertain);
  RUN(restart_mid_session_keeps_real_timestamps_but_is_labelled_uncertain);
  RUN(log_expiry_closes_the_session_and_marks_the_gap);
  RUN(log_reset_with_fewer_entries_is_detected);
  RUN(log_reset_refilled_past_the_cursor_is_caught_by_the_anchor_on_restart);
  RUN(log_that_shrinks_and_regrows_is_caught_by_range_movement);
  RUN(garbage_range_changes_nothing_and_backs_off);
  RUN(pdm_errors_back_off_without_losing_events);
  RUN(backoff_is_capped);
  RUN(steady_clock_failure_does_not_stop_recording);
  RUN(init_rejects_missing_callbacks);
  return t_done("watcher");
}
