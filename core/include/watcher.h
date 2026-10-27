#ifndef AW_WATCHER_H
#define AW_WATCHER_H

#include <stddef.h>
#include <stdint.h>

#include "pdm_layout.h"

/* Raw 0x38 byte play-log entry, copied unchanged from pdm:qry. Padded to 56 bytes. */
typedef struct { uint8_t b[56]; } RawEvent;

typedef struct {
  int (*range)(void *u, int32_t *total, int32_t *start, int32_t *end);          /* 0 or errno-like code */
  int (*query)(void *u, int32_t idx, RawEvent *out, int32_t max, int32_t *n);   /* 0 or code */
} PdmSrc;

/* All paths come from paths.h. All Fs functions return 0 or an errno-like code. */
typedef struct {
  int (*append_line)(void *u, const char *path, const char *buf, size_t n);     /* open append, write, fflush, fsync, close; n=0 still flushes and fsyncs */
  int (*read_tail)(void *u, const char *path, char *buf, size_t cap, size_t *n);/* last <= cap bytes. ENOENT if absent */
  int (*write_file)(void *u, const char *path, const void *buf, size_t n);      /* create/truncate, write, fsync, close. n may be 0 */
  int (*read_file)(void *u, const char *path, void *buf, size_t cap, size_t *n);/* ENOENT if absent */
} Fs;

typedef struct {
  int (*steady)(void *u, int64_t *secs, uint8_t source_id[16]);
  int (*diag_enabled)(void *u);                            /* nonzero when the diag flag file exists */
  int (*diag_write)(void *u, const char *line, size_t n);  /* best effort. A nonzero return disables diag until restart */
} Env;

/* Caller provides storage of watcher_size() bytes, aligned to WATCHER_ALIGN, kept off the thread stack.
 * A static WatcherStorage works: it is large enough for every build (checked by _Static_assert in watcher.c). */
#define WATCHER_ALIGN 16
#define WATCHER_STORAGE_BYTES 12288
typedef struct Watcher Watcher;
typedef union { max_align_t a; uint8_t b[WATCHER_STORAGE_BYTES]; } WatcherStorage;

size_t watcher_size(void);

/* Loads state and journal tail (recovery, see docs/FORMAT.md). Returns 0 unless an argument is NULL.
 * Recovery that needs a working SD card or pdm is retried inside watcher_poll with backoff. */
int watcher_init(Watcher *w, PdmSrc, void *pu, Fs, void *fu, Env, void *eu);

/* One poll. Always returns 0 for operational problems (they back off and set *next_sleep_ms). */
int watcher_poll(Watcher *w, uint32_t *next_sleep_ms);

/* Diag "dump_all": writes every log entry from start to tail into the diag log. Read-only on journal and state. */
int watcher_dump_all(Watcher *w);

#endif
