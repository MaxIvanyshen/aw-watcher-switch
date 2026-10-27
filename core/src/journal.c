#include "journal.h"

#include <errno.h>
#include <string.h>

#include "paths.h"

void journal_init(Journal *j, const Fs *fs, void *fu) {
  memset(j, 0, sizeof *j);
  j->fs = fs;
  j->fu = fu;
}

int journal_sync(Journal *j) {
  size_t n = 0;
  int rc = j->fs->read_tail(j->fu, AW_PATH_SESSIONS, j->tail, sizeof j->tail, &n);
  if (rc == ENOENT) { j->exists = 0; j->fence = 0; j->dirty = 0; return 0; }
  if (rc != 0) { j->dirty = 1; return rc; }
  if (n > sizeof j->tail) n = sizeof j->tail;
  Key k;
  int ends_nl;
  int found = jsonl_last_key(j->tail, n, n < sizeof j->tail, &k, &ends_nl);
  j->exists = n > 0;
  j->fence = !ends_nl;
  /* A readable line may still be in cache after a failed fsync, including across a
   * process restart. Sync this file before its key can suppress replay/state writes. */
  if (found) {
    rc = j->fs->append_line(j->fu, AW_PATH_SESSIONS, "", 0);
    if (rc) { j->dirty = 1; return rc; }
  }
  if (found && (!j->last_ok || key_cmp(&k, &j->last) > 0)) { j->last = k; j->last_ok = 1; }
  j->dirty = 0;
  return 0;
}

int journal_emit(Journal *j, const Key *k, char *line, size_t n) {
  if (j->dirty) {
    int rc = journal_sync(j);
    if (rc) return rc;
  }
  if (j->last_ok && key_cmp(k, &j->last) <= 0) { j->suppressed++; return 0; }
  const char *p = line + 1;
  size_t len = n;
  if (j->fence) { line[0] = '\n'; p = line; len = n + 1; }
  int rc = j->fs->append_line(j->fu, AW_PATH_SESSIONS, p, len);
  if (rc) { j->dirty = 1; return rc; }
  j->last = *k; j->last_ok = 1; j->exists = 1; j->fence = 0;
  return 0;
}
