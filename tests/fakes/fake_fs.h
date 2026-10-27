#ifndef FAKE_FS_H
#define FAKE_FS_H

#include <setjmp.h>

#include "watcher.h"

#define FF_FILES 5
#define FF_CAP 131072

typedef struct { char path[96]; int used; size_t n; uint8_t d[FF_CAP]; } FFile;

typedef enum { CM_NONE = -1, CM_BEFORE = 0, CM_TORN = 1, CM_AFTER = 2, CM_ZERO = 3, CM_TRUNC = 4, CM_COUNT = 5 } CrashMode;
typedef enum { FK_CLEAN = 0, FK_PARTIAL = 1, FK_LANDED = 2 } FaultKind;

typedef struct FakeFs {
  FFile files[FF_FILES];
  uint32_t ops;            /* mutating ops so far (append_line and write_file) */
  int crash_at;            /* absolute op index to crash at, -1 = none */
  CrashMode crash_mode;
  jmp_buf *jb;
  int fault_at, fault_len; /* ops [fault_at, fault_at + fault_len) fail */
  FaultKind fault_kind;
  int fault_errno;
  char read_err_path[96];  /* reads of paths containing this text fail with read_err (read_err_n times, -1 = always) */
  int read_err, read_err_n;
  uint32_t appends, writes;
} FakeFs;

void ffs_init(FakeFs *f);
Fs ffs_iface(void);
FFile *ffs_find(FakeFs *f, const char *path);
FFile *ffs_get(FakeFs *f, const char *path);              /* creates */
void ffs_set(FakeFs *f, const char *path, const void *data, size_t n);
void ffs_remove(FakeFs *f, const char *path);
void ffs_crash_plan(FakeFs *f, int at, CrashMode m, jmp_buf *jb);
void ffs_clear_plans(FakeFs *f);

#endif
