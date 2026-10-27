#include "tracker.h"

#include <string.h>

void tracker_reset(Tracker *t) { memset(t, 0, sizeof *t); }

void tracker_touch(Tracker *t, uint64_t now) {
  if (t->open && now > t->alive && now - t->alive <= ALIVE_GAP_MAX) t->alive = now;
}

static void finish(Tracker *t, SessionRec *r, CloseWhy why, Sem sem) {
  r->pid = t->pid; r->su = t->su; r->sn = t->sn; r->ss = t->ss; r->pol = t->pol;
  r->why = (uint8_t)why;
  r->dur_ok = r->ss != 0 && r->es != 0 && r->es >= r->ss;
  r->dur = r->dur_ok ? r->es - r->ss : 0;
  r->sem = (uint8_t)(r->dur_ok ? sem : SEM_UNC);
  t->open = 0;
}

int tracker_close_unc(Tracker *t, CloseWhy why, SessionRec *r) {
  if (!t->open) return 0;
  memset(r, 0, sizeof *r);
  r->es = t->alive;
  finish(t, r, why, SEM_UNC);
  return 1;
}

static void close_at_event(Tracker *t, const Event *e, SessionRec *r, CloseWhy why, Sem sem) {
  memset(r, 0, sizeof *r);
  r->eu = e->ts_user; r->en = e->ts_net; r->es = e->ts_steady; r->end_known = 1;
  if (r->es > t->alive && r->es - t->alive > ALIVE_GAP_MAX) sem = SEM_UNC;
  finish(t, r, why, sem);
}

static void open_at_event(Tracker *t, const Event *e) {
  t->open = 1; t->pol = e->log_policy; t->pid = e->pid;
  t->su = e->ts_user; t->sn = e->ts_net; t->ss = e->ts_steady; t->alive = e->ts_steady;
}

static Note applet(Tracker *t, const Event *e, TrackOut *o) {
  if (e->applet_id != APPLET_ID_APPLICATION) return NOTE_IGN_APPLET_ID;
  if (e->log_policy == POLICY_NONE) return NOTE_IGN_POLICY;
  int match = t->open && t->pid == e->pid;
  switch (e->applet_event) {
  case AE_IN_FOCUS:
    if (!t->open) { open_at_event(t, e); return NOTE_OPEN; }
    if (match) return NOTE_DUP_FOCUS;
    /* No out_of_focus was logged for the old program: its end is inferred, so never "ok". */
    close_at_event(t, e, &o->rec, WHY_SUPERSEDED, SEM_UNC);
    o->has_rec = 1;
    open_at_event(t, e);
    return NOTE_SUPERSEDE;
  case AE_OUT_OF_FOCUS: case AE_OUT_OF_FOCUS4:
    if (!match) return NOTE_IGN_NOMATCH;
    close_at_event(t, e, &o->rec, WHY_OUT_OF_FOCUS, SEM_OK);
    o->has_rec = 1;
    return NOTE_CLOSE;
  case AE_EXIT: case AE_EXIT5: case AE_EXIT6:
    if (!match) return NOTE_IGN_NOMATCH;
    close_at_event(t, e, &o->rec, WHY_EXIT, SEM_OK);
    o->has_rec = 1;
    return NOTE_CLOSE;
  case AE_LAUNCH: return NOTE_IGN_LAUNCH;
  default: return NOTE_IGN_UNKNOWN;
  }
}

void tracker_step(Tracker *t, const Event *e, TrackOut *o) {
  memset(o, 0, sizeof *o);
  if (e->ts_steady != 0) {
    if (t->prev_valid && e->ts_steady < t->prev_steady) {
      o->has_rec = tracker_close_unc(t, WHY_CLOCK_RESET, &o->rec);
      tracker_reset(t);
      o->bump = 1;
    }
    t->prev_steady = e->ts_steady;
    t->prev_valid = 1;
  }
  switch (e->play_type) {
  case PT_APPLET: o->note = applet(t, e, o); break;
  case PT_ACCOUNT: o->note = NOTE_IGN_ACCOUNT; break;
  case PT_OPMODE: o->note = NOTE_IGN_OPMODE; break;
  case PT_POWER:
    if (WATCHER_POWER_POLICY_CLOSE_ANY && t->open) {
      close_at_event(t, e, &o->rec, WHY_POWER, SEM_EXP);
      o->has_rec = 1;
      o->note = NOTE_CLOSE;
    } else o->note = NOTE_IGN_POWER;
    break;
  case PT_INIT:
    /* Initialize is the first entry of a fresh log. The time since the last observation is unknown (R8),
     * so the session ends at the last time it was seen alive, not at this event. */
    if (t->open) {
      o->has_rec = tracker_close_unc(t, WHY_INIT, &o->rec);
      o->note = NOTE_CLOSE;
    } else o->note = NOTE_IGN_INIT;
    break;
  default: o->note = NOTE_IGN_UNKNOWN; break;
  }
  if (e->ts_steady != 0) tracker_touch(t, e->ts_steady);
}

const char *close_why_name(unsigned why) {
  static const char *n[WHY_COUNT] = {"out_of_focus", "exit", "superseded", "power", "init", "clock_reset", "lost"};
  return why < WHY_COUNT ? n[why] : "?";
}

const char *sem_name(unsigned sem) {
  static const char *n[3] = {"ok", "exp", "unc"};
  return sem < 3 ? n[sem] : "?";
}

const char *note_name(Note n) {
  static const char *names[NOTE_COUNT] = {
    "open", "close", "supersede", "dup_focus", "ign_applet_id", "ign_policy", "ign_nomatch",
    "ign_launch", "ign_account", "ign_opmode", "ign_power", "ign_init", "ign_unknown"};
  return (unsigned)n < NOTE_COUNT ? names[n] : "?";
}
