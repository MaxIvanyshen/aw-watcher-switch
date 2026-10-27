#ifndef AW_DIAG_H
#define AW_DIAG_H

#include "tracker.h"

#define DIAG_LINE_MAX 512
#define DIAG_CAP_BYTES (2u * 1024u * 1024u)

typedef struct {
  const Env *env;
  void *eu;
  uint8_t on;    /* flag present at the start of this poll */
  uint8_t dead;  /* a write failed or the cap was hit: off until restart */
  uint32_t bytes;
  char line[DIAG_LINE_MAX];
} Diag;

typedef struct {
  uint32_t events, ignored, dup_focus, io_err, epochs;
} DiagStats;

void diag_init(Diag *d, const Env *env, void *eu);
void diag_begin(Diag *d); /* refreshes d->on from the flag file */
int diag_active(const Diag *d);

void diag_boot(Diag *d, const char *mode, uint32_t ep, int32_t cursor);
void diag_range(Diag *d, int rc, int32_t total, int32_t start, int32_t end);
void diag_probe(Diag *d, int32_t idx, int rc, int32_t n);
void diag_raw(Diag *d, const char *kind, int32_t idx, const RawEvent *r, const Event *e, const TrackOut *o);
void diag_tr(Diag *d, const char *what, const char *why, uint32_t ep, int32_t a, int32_t b);
void diag_io(Diag *d, const char *op, int rc, uint32_t backoff_ms);
void diag_stats(Diag *d, const DiagStats *s);

#endif
