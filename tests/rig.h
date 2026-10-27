#ifndef RIG_H
#define RIG_H

#include "fakes/fake_env.h"
#include "fakes/fake_fs.h"
#include "fakes/fake_pdm.h"
#include "jsonl.h"
#include "paths.h"
#include "watcher.h"

#define POLL_MS_EXPECT 5000u
#define BUSY_MS_EXPECT 100u

typedef struct Rig {
  FakeFs fs;
  FakePdm *pdm;
  FakeEnv env;
  WatcherStorage store;
  Watcher *w;
  jmp_buf jb;
  int crashes;
  uint32_t last_sleep;
  void (*after_crash)(struct Rig *);   /* called once the crashed process was rebooted */
  void *user;
} Rig;

Rig *rig_new(void);                          /* Rig is large: keep it on the heap */
void rig_del(Rig *r);
void rig_init(Rig *r);
void rig_free(Rig *r);
void rig_boot(Rig *r);                       /* new process over the same SD card and the same pdm log */
int rig_poll(Rig *r);                        /* one poll. Returns 1 if the process crashed (it is rebooted) */
void rig_settle(Rig *r);                     /* poll until a poll does no writes */
void rig_at(Rig *r, int64_t steady);         /* set the steady clock */
void rig_tick(Rig *r, int64_t to);           /* advance the clock to `to` in steps of at most 10 s, settling at each */
void rig_push(Rig *r, RawEvent e);

/* Journal inspection. Lines are NUL terminated copies. */
#define RJ_MAX_LINES 1024
typedef struct {
  int n_lines, n_valid, n_bad;           /* bad = non-empty and not valid */
  int unterminated;                      /* last line has no '\n' */
  char (*line)[JSONL_MAX + 1];           /* valid lines only */
  Key *key;
  int sessions, gaps;
} Journal_View;

void jv_load(Rig *r, Journal_View *v);
void jv_free(Journal_View *v);
int jv_keys_increasing(const Journal_View *v);

/* Field access on a line. */
int rec_num(const char *line, const char *field, uint64_t *out);   /* 0 if absent or null */
int rec_is_null(const char *line, const char *field);
int rec_str(const char *line, const char *field, char *out, size_t cap);
int rec_is_session(const char *line);
int jv_find(const Journal_View *v, int from, const char *needle);   /* index of the next valid line containing needle, or -1 */
int jv_count(const Journal_View *v, const char *needle);

/* Equivalence of two journals produced by the same history (reference vs. crashed and recovered).
 * Same keys, same records. Fields that depend on when the watcher polled (es/dur of records whose end was not
 * observed) are skipped. A recovered run may be more cautious (higher sem) than the reference, never less.
 * Returns 1 if equivalent, else 0 with a message in why. */
int jv_equivalent(const Journal_View *ref, const Journal_View *got, char *why, size_t cap);

/* Structural invariants of any journal: unique increasing keys, sessions never overlap inside an epoch,
 * dur == es - ss, sem consistent with why and end_known. Returns 1 if all hold. */
int jv_invariants(const Journal_View *v, char *why, size_t cap);

/* Raw file access helpers */
void snapshot_state(Rig *r, uint8_t *a, size_t *an, uint8_t *b, size_t *bn);

#endif
