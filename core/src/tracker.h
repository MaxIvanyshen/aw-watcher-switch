#ifndef AW_TRACKER_H
#define AW_TRACKER_H

#include "decode.h"

/* Compile-time power policy. 1 = any PowerStateChange closes the open session (E1, conservative). */
#ifndef WATCHER_POWER_POLICY_CLOSE_ANY
#define WATCHER_POWER_POLICY_CLOSE_ANY 1
#endif

/* Largest steady-clock step (seconds) across which the watcher still vouches that a session was running.
 * A longer step means the watcher was suspended, stalled or off, so the time in it is unobserved. */
#define ALIVE_GAP_MAX 30

typedef enum { SEM_OK = 0, SEM_EXP = 1, SEM_UNC = 2 } Sem;
typedef enum {
  WHY_OUT_OF_FOCUS, WHY_EXIT, WHY_SUPERSEDED, WHY_POWER, WHY_INIT, WHY_CLOCK_RESET, WHY_LOST, WHY_COUNT
} CloseWhy;

typedef struct {
  uint64_t pid, su, sn, ss, eu, en, es, dur;
  uint8_t pol, why, sem;
  uint8_t end_known; /* eu/en/es come from a log event. Otherwise es is the last time the watcher saw the session alive */
  uint8_t dur_ok;
} SessionRec;

typedef struct {
  uint8_t open, pol, prev_valid;
  uint64_t pid, su, sn, ss;
  uint64_t alive;       /* last steady time at which the log was read with this session still open */
  uint64_t prev_steady;
} Tracker;

typedef enum {
  NOTE_OPEN, NOTE_CLOSE, NOTE_SUPERSEDE, NOTE_DUP_FOCUS, NOTE_IGN_APPLET_ID, NOTE_IGN_POLICY, NOTE_IGN_NOMATCH,
  NOTE_IGN_LAUNCH, NOTE_IGN_ACCOUNT, NOTE_IGN_OPMODE, NOTE_IGN_POWER, NOTE_IGN_INIT, NOTE_IGN_UNKNOWN, NOTE_COUNT
} Note;

typedef struct {
  int has_rec;
  SessionRec rec;
  int bump;  /* steady clock went backwards: the caller starts a new epoch, the event is already applied to the fresh tracker */
  Note note;
} TrackOut;

void tracker_reset(Tracker *t);
void tracker_step(Tracker *t, const Event *e, TrackOut *o);
void tracker_touch(Tracker *t, uint64_t steady_now);
int tracker_close_unc(Tracker *t, CloseWhy why, SessionRec *r); /* 1 if a session was open */

const char *close_why_name(unsigned why);
const char *sem_name(unsigned sem);
const char *note_name(Note n);

#endif
