#include "sync_priv.h"
#include <errno.h>
#include <string.h>

static int ci_eq(char a, char b) {
  if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
  if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
  return a == b;
}

int sync_path_ok(const char *p) {
  size_t n = strlen(p), i, start;
  if (n < 2 || n >= 96 || p[0] != '/' || p[n-1] == '/') return 0;
  for (i = 0; i < n; i++) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/')) return 0;
  }
  for (start = 1; start <= n; ) {
    size_t end = start;
    while (end < n && p[end] != '/') end++;
    if (end == start) return 0;
    if (end - start == 1 && p[start] == '.') return 0;
    if (end - start == 2 && p[start] == '.' && p[start+1] == '.') return 0;
    start = end + 1;
  }
  return 1;
}

int sync_path_under(const char *path, const char *dir) {
  size_t n = strlen(dir), i;
  for (i = 0; i < n; i++) if (!path[i] || !ci_eq(path[i], dir[i])) return 0;
  return path[n] == 0 || path[n] == '/';
}

static int dirname_of(const char *p, char *out, size_t cap) {
  const char *s = strrchr(p, '/');
  size_t n = (size_t)(s - p);
  if (n == 0 || n >= cap) return -EINVAL;
  memcpy(out, p, n); out[n] = 0;
  return 0;
}

int sync_cfg_paths_ok(const SyncCfg *c) {
  char wd[96], jd[96];
  if (!sync_path_ok(c->journal) || !sync_path_ok(c->state_dir)) return -EINVAL;
  if (dirname_of(SYNC_PATH_JOURNAL, wd, sizeof wd) || dirname_of(c->journal, jd, sizeof jd)) return -EINVAL;
  /* the uploader may only write below state_dir: never inside the watcher directory, never over the journal */
  if (sync_path_under(c->state_dir, wd) || sync_path_under(c->journal, c->state_dir)) return -EINVAL;
  if (strlen(jd) == strlen(c->state_dir) && sync_path_under(jd, c->state_dir)) return -EINVAL;
  return 0;
}

int sync_cfg_validate(const SyncCfg *c) {
  size_t i;
  if (c->ip[0] != 100 || c->ip[1] < 64 || c->ip[1] > 127 || c->port == 0) return -EINVAL;
  if (strlen(c->device) != 36) return -EINVAL;
  for (i = 0; i < 36; i++) {
    char ch = c->device[i];
    int dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? ch != '-' : !((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return -EINVAL;
  }
  i = strlen(c->token);
  if (i < 16 || i > 128) return -EINVAL;
  for (i = 0; c->token[i]; i++) {
    char ch = c->token[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '~' || ch == '+' || ch == '-')) return -EINVAL;
  }
  return sync_cfg_paths_ok(c);
}

static int num(const char **p, const char *end, unsigned max, unsigned *out) {
  unsigned v = 0, d = 0;
  const char *s = *p;
  while (s < end && *s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); if (v > max) return -1; s++; d++; }
  if (d == 0 || d > 5 || (d > 1 && **p == '0')) return -1;
  *p = s; *out = v;
  return 0;
}

static int parse_server(const char *v, const char *end, SyncCfg *c) {
  unsigned a, port;
  for (int i = 0; i < 4; i++) {
    if (num(&v, end, 255, &a)) return -EINVAL;
    c->ip[i] = (uint8_t)a;
    if (i < 3) { if (v >= end || *v != '.') return -EINVAL; v++; }
  }
  if (v >= end || *v != ':') return -EINVAL;
  v++;
  if (num(&v, end, 65535, &port) || v != end) return -EINVAL;
  c->port = (uint16_t)port;
  return 0;
}

static int copy(char *dst, size_t cap, const char *v, size_t n) {
  if (n == 0 || n >= cap) return -EINVAL;
  memcpy(dst, v, n); dst[n] = 0;
  return 0;
}

int sync_cfg_parse(const char *text, size_t n, SyncCfg *out) {
  SyncCfg c;
  unsigned seen = 0;
  size_t i = 0;
  static const char *const keys[] = {"server", "device", "token", "journal", "state_dir"};
  if (n > 512) return -EINVAL;
  if (memchr(text, 0, n) || memchr(text, '\r', n)) return -EINVAL;
  memset(&c, 0, sizeof c);
  strcpy(c.journal, SYNC_PATH_JOURNAL);
  strcpy(c.state_dir, SYNC_DATA_DIR);
  while (i < n) {
    const char *ls = text + i, *nl = memchr(ls, '\n', n - i), *le = nl ? nl : text + n, *eq = memchr(ls, '=', (size_t)(le - ls));
    size_t k, kl;
    int rc = -EINVAL;
    if (!eq) return -EINVAL;
    kl = (size_t)(eq - ls);
    for (k = 0; k < 5; k++) if (strlen(keys[k]) == kl && !memcmp(keys[k], ls, kl)) break;
    if (k == 5 || (seen & (1u << k))) return -EINVAL;
    seen |= 1u << k;
    switch (k) {
      case 0: rc = parse_server(eq + 1, le, &c); break;
      case 1: rc = copy(c.device, sizeof c.device, eq + 1, (size_t)(le - eq - 1)); break;
      case 2: rc = copy(c.token, sizeof c.token, eq + 1, (size_t)(le - eq - 1)); break;
      case 3: rc = copy(c.journal, sizeof c.journal, eq + 1, (size_t)(le - eq - 1)); break;
      default: rc = copy(c.state_dir, sizeof c.state_dir, eq + 1, (size_t)(le - eq - 1)); break;
    }
    if (rc) return rc;
    i = (size_t)(le - text) + 1;
  }
  if ((seen & 7u) != 7u) return -EINVAL;
  if (sync_cfg_validate(&c)) return -EINVAL;
  *out = c;
  return 0;
}
