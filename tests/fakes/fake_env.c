#include "fake_env.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void fenv_init(FakeEnv *e) {
  memset(e, 0, sizeof *e);
  e->diag_fail_after = -1;
  e->diag = malloc(FE_DIAG_CAP);
  if (!e->diag) abort();
  e->src[0] = 1;
}

void fenv_free(FakeEnv *e) { free(e->diag); e->diag = NULL; }

static int env_steady(void *u, int64_t *secs, uint8_t src[16]) {
  FakeEnv *e = u;
  if (e->steady_err) return e->steady_err;
  *secs = e->steady;
  memcpy(src, e->src, 16);
  return 0;
}

static int env_diag_enabled(void *u) { return ((FakeEnv *)u)->diag_flag; }

static int env_diag_write(void *u, const char *line, size_t n) {
  FakeEnv *e = u;
  if (e->diag_fail_after >= 0 && e->diag_lines >= (uint32_t)e->diag_fail_after) return EIO;
  if (e->diag_len + n + 1 > FE_DIAG_CAP) return ENOSPC;
  memcpy(e->diag + e->diag_len, line, n);
  e->diag_len += n;
  e->diag[e->diag_len] = 0;
  e->diag_lines++;
  return 0;
}

Env fenv_iface(void) {
  Env v = {env_steady, env_diag_enabled, env_diag_write};
  return v;
}
