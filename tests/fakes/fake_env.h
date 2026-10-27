#ifndef FAKE_ENV_H
#define FAKE_ENV_H

#include "watcher.h"

#define FE_DIAG_CAP (4u * 1024u * 1024u)

typedef struct {
  int64_t steady;
  uint8_t src[16];
  int steady_err;
  int diag_flag;
  int diag_fail_after;   /* diag_write fails once this many lines were written, -1 = never */
  char *diag;            /* captured diag lines */
  size_t diag_len;
  uint32_t diag_lines;
} FakeEnv;

void fenv_init(FakeEnv *e);
void fenv_free(FakeEnv *e);
Env fenv_iface(void);

#endif
