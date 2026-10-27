#ifndef AWSYNC_FAKES_H
#define AWSYNC_FAKES_H
#include "sync_engine.h"
#include <stddef.h>
#include <stdint.h>

#define FK_FILES 12
#define FK_RECS  4096
#define FK_BATCHES 256

typedef struct { char path[128]; uint8_t *d; size_t n; int used; } FkFile;
typedef struct { long ep, ev, sub; char line[SYNC_LINE_MAX + 1]; } FkRec;
typedef struct { char id[48]; char sha[65]; char ack[200]; } FkBatch;

typedef enum {
  M_OK = 0, M_DROP_RESP, M_WRONG_ID, M_WRONG_SHA, M_WRONG_COUNT, M_BAD_SUM, M_S500, M_TRUNC, M_BIG_CL, M_EXTRA,
  M_OLD_ACK, M_NO_CL, M_CHUNKED, M_S400, M_S401, M_S403, M_S409, M_S413, M_GARBAGE
} FkMode;

typedef struct Fake Fake;
struct Fake {
  FkFile files[FK_FILES];
  uint64_t now, exch_advance;
  int calls, fail_at, fail_err;              /* fail the fail_at-th failable platform call (1-based) */
  int fail_write_prefix;                     /* every write_at to a path starting with this index fails: 1 = ckpt slots */
  int read_fail_after;                       /* >0: journal read_at starts failing after that many reads */
  int journal_reads; int journal_read_err;
  int net_open_n, net_close_n, exch_n, net_fail;
  FkMode mode; int mode_left;                /* mode applies to the next mode_left exchanges (<0 forever) */
  FkRec *recs; size_t nrecs;
  FkBatch batches[FK_BATCHES]; size_t nbatches;
  uint32_t last_new, last_dup;
  uint8_t last_body_sha[32]; char last_id[48];
  char sent_ids[FK_BATCHES][48]; size_t nsent;
  char wlog[256][128]; size_t nw;
  const char *token, *device;
  void (*hook)(Fake *f, int when, int exch_idx);   /* when: 0 before the server sees the request, 1 after commit */
};

void fk_init(Fake *f, const char *token, const char *device);
void fk_free(Fake *f);
void fk_plat(Fake *f, SyncPlat *p);
void fk_put(Fake *f, const char *path, const void *d, size_t n);
void fk_append(Fake *f, const char *path, const void *d, size_t n);
const uint8_t *fk_get(Fake *f, const char *path, size_t *n);
void fk_del(Fake *f, const char *path);
size_t fk_nrecs(const Fake *f);
int fk_has(const Fake *f, long ep, long ev, long sub);
/* Every write went below state_dir, none to the watcher dir or journal. */
int fk_writes_clean(const Fake *f, const char *state_dir, const char *journal);
#endif
