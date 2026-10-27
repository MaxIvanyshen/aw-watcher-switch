#include "fake_fs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void ffs_init(FakeFs *f) {
  for (int i = 0; i < FF_FILES; i++) f->files[i].used = 0;
  f->ops = f->appends = f->writes = 0;
  f->jb = NULL;
  f->fault_len = 0; f->fault_kind = FK_CLEAN; f->fault_errno = 0;
  f->read_err_path[0] = 0; f->read_err = f->read_err_n = 0;
  f->crash_at = -1;
  f->crash_mode = CM_NONE;
  f->fault_at = -1;
}

FFile *ffs_find(FakeFs *f, const char *path) {
  for (int i = 0; i < FF_FILES; i++)
    if (f->files[i].used && strcmp(f->files[i].path, path) == 0) return &f->files[i];
  return NULL;
}

FFile *ffs_get(FakeFs *f, const char *path) {
  FFile *x = ffs_find(f, path);
  if (x) return x;
  for (int i = 0; i < FF_FILES; i++)
    if (!f->files[i].used) {
      f->files[i].used = 1;
      f->files[i].n = 0;
      strncpy(f->files[i].path, path, sizeof f->files[i].path - 1);
      return &f->files[i];
    }
  abort();
}

void ffs_set(FakeFs *f, const char *path, const void *data, size_t n) {
  FFile *x = ffs_get(f, path);
  memcpy(x->d, data, n);
  x->n = n;
}

void ffs_remove(FakeFs *f, const char *path) {
  FFile *x = ffs_find(f, path);
  if (x) x->used = 0;
}

void ffs_crash_plan(FakeFs *f, int at, CrashMode m, jmp_buf *jb) { f->crash_at = at; f->crash_mode = m; f->jb = jb; }

void ffs_clear_plans(FakeFs *f) {
  f->crash_at = -1; f->crash_mode = CM_NONE; f->jb = NULL; f->fault_at = -1; f->fault_len = 0; f->read_err = 0; f->read_err_n = 0;
}

static void put(FFile *x, size_t at, const void *p, size_t n) {
  if (at + n > FF_CAP) abort();
  memcpy(x->d + at, p, n);
  x->n = at + n > x->n ? at + n : x->n;
}

/* One mutating op. append: data goes after the existing bytes. write: file is replaced. */
static int mutate(FakeFs *f, const char *path, const void *buf, size_t n, int append) {
  uint32_t op = f->ops++;
  FFile *x = ffs_get(f, path);
  size_t base = append ? x->n : 0;
  static const uint8_t zeros[1024];

  if (f->crash_at >= 0 && (uint32_t)f->crash_at == op && f->crash_mode != CM_NONE) {
    switch (f->crash_mode) {
    case CM_BEFORE: break;
    case CM_TORN: if (!append) x->n = 0; put(x, base, buf, n / 2); break;
    case CM_AFTER: if (!append) x->n = 0; put(x, base, buf, n); break;
    case CM_ZERO: if (!append) x->n = 0; put(x, base, zeros, n < sizeof zeros ? n : sizeof zeros); break;
    case CM_TRUNC: if (!append) x->n = 0; break;
    default: break;
    }
    longjmp(*f->jb, 1);
  }
  if (f->fault_at >= 0 && (int)op >= f->fault_at && (int)op < f->fault_at + f->fault_len) {
    switch (f->fault_kind) {
    case FK_CLEAN: break;
    case FK_PARTIAL: if (!append) x->n = 0; put(x, base, buf, n / 2); break;
    case FK_LANDED: if (!append) x->n = 0; put(x, base, buf, n); break;
    }
    return f->fault_errno ? f->fault_errno : EIO;
  }
  if (!append) x->n = 0;
  put(x, base, buf, n);
  if (append) f->appends++; else f->writes++;
  return 0;
}

static int read_fault(FakeFs *f, const char *path) {
  if (f->read_err && f->read_err_n != 0 && strstr(path, f->read_err_path)) {
    if (f->read_err_n > 0) f->read_err_n--;
    return f->read_err;
  }
  return 0;
}

static int fs_append(void *u, const char *path, const char *buf, size_t n) { return mutate(u, path, buf, n, 1); }
static int fs_write(void *u, const char *path, const void *buf, size_t n) { return mutate(u, path, buf, n, 0); }

static int fs_read_tail(void *u, const char *path, char *buf, size_t cap, size_t *n) {
  FakeFs *f = u;
  int rc = read_fault(f, path);
  if (rc) return rc;
  FFile *x = ffs_find(f, path);
  if (!x) return ENOENT;
  size_t take = x->n < cap ? x->n : cap;
  memcpy(buf, x->d + (x->n - take), take);
  *n = take;
  return 0;
}

static int fs_read_file(void *u, const char *path, void *buf, size_t cap, size_t *n) {
  FakeFs *f = u;
  int rc = read_fault(f, path);
  if (rc) return rc;
  FFile *x = ffs_find(f, path);
  if (!x) return ENOENT;
  size_t take = x->n < cap ? x->n : cap;
  memcpy(buf, x->d, take);
  *n = take;
  return 0;
}

Fs ffs_iface(void) {
  Fs fs = {fs_append, fs_read_tail, fs_write, fs_read_file};
  return fs;
}
