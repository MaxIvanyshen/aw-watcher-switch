#define _POSIX_C_SOURCE 200809L
#include "posix_plat.h"
#include "sync_engine.h"
#include "sync_priv.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static SyncEngine eng;

typedef struct { long ep, ev, sub; } Key;

static int kcmp(const void *a, const void *b) {
  const Key *x = a, *y = b;
  if (x->ep != y->ep) return x->ep < y->ep ? -1 : 1;
  if (x->ev != y->ev) return x->ev < y->ev ? -1 : 1;
  return x->sub < y->sub ? -1 : x->sub > y->sub;
}

static long field(const char *l, const char *name) {
  const char *p = strstr(l, name);
  return p ? strtol(p + strlen(name), NULL, 10) : 0;
}

/* Same line rules as the engine: CRC-valid lines only, garbage skipped, an unterminated valid last line counts. */
static int scan_file(const char *path) {
  FILE *f = fopen(path, "rb");
  char line[SYNC_LINE_MAX + 2];
  Key *k = NULL;
  size_t n = 0, cap = 0, i;
  int c;
  size_t len = 0, over = 0;
  if (!f) { perror(path); return 1; }
  for (;;) {
    c = fgetc(f);
    if (c == '\n' || c == EOF) {
      if (!over && len && spool_line_valid((const uint8_t *)line, len)) {
        line[len] = 0;
        if (n == cap) { cap = cap ? cap * 2 : 64; k = realloc(k, cap * sizeof *k); if (!k) return 1; }
        k[n].ep = field(line, "\"ep\":"); k[n].ev = field(line, "\"ev\":"); k[n].sub = field(line, "\"sub\":"); n++;
      }
      len = 0; over = 0;
      if (c == EOF) break;
    } else if (len < SYNC_LINE_MAX) line[len++] = (char)c;
    else over = 1;
  }
  fclose(f);
  qsort(k, n, sizeof *k, kcmp);
  for (i = 0; i < n; i++) printf("%ld %ld %ld\n", k[i].ep, k[i].ev, k[i].sub);
  free(k);
  return 0;
}

static void mkdirs(const char *root, const char *dir) {
  char p[512], *s;
  snprintf(p, sizeof p, "%s%s", root, dir);
  for (s = p + strlen(root) + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0700); *s = '/'; }
  mkdir(p, 0700);
}

int main(int argc, char **argv) {
  const char *root = NULL, *conf = NULL;
  int once = 0;
  const char *over = NULL;
  char text[1024];
  size_t n;
  FILE *f;
  SyncCfg cfg;
  SyncPlat plat;
  PosixCtx px;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--scan") && i + 1 < argc) return scan_file(argv[i + 1]);
    else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--config") && i + 1 < argc) conf = argv[++i];
    else if (!strcmp(argv[i], "--once")) once = 1;
    else if (!strcmp(argv[i], "--connect") && i + 1 < argc) over = argv[++i];
    else { fprintf(stderr, "usage: sync_host --scan JOURNAL | --root DIR --config FILE [--once] [--connect IP:PORT]\n"); return 64; }
  }
  if (!root || !conf) { fprintf(stderr, "need --root and --config\n"); return 64; }
  f = fopen(conf, "rb");
  if (!f) { perror(conf); return 64; }
  n = fread(text, 1, sizeof text, f);
  fclose(f);
  if (sync_cfg_parse(text, n, &cfg) != 0) { fprintf(stderr, "invalid config\n"); return 3; }
  mkdirs(root, cfg.state_dir);
  posix_plat_init(&plat, &px, root);
  if (over) {
    unsigned a, b, c2, d, port;
    if (sscanf(over, "%u.%u.%u.%u:%u", &a, &b, &c2, &d, &port) != 5 || port > 65535) return 64;
    px.override = 1; px.oip[0] = (uint8_t)a; px.oip[1] = (uint8_t)b; px.oip[2] = (uint8_t)c2; px.oip[3] = (uint8_t)d; px.oport = (uint16_t)port;
  }
  sync_init(&eng, &plat, &cfg);
  for (;;) {
    SyncRun r = sync_run_once(&eng);
    fprintf(stderr, "sync: %s/%s http=%d errno=%d offset=%llu acked=%llu\n", eng.stage, eng.result, eng.last_http,
            eng.last_errno, (unsigned long long)eng.ck.offset, (unsigned long long)eng.ck.acked);
    if (r.outcome == SYNC_MORE) continue;
    if (once) return r.outcome == SYNC_IDLE ? 0 : r.outcome == SYNC_RETRY ? 2 : 3;
    usleep(r.delay_ms * 1000u);
  }
}
