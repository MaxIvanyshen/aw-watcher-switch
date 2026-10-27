#include <errno.h>

#include "rig.h"
#include "state.h"
#include "t.h"

/* Targeted Fs wrapper: the base image is readable, but an append whose fsync
 * failed is not retained on power loss. State writes remain independently durable. */
typedef struct {
  FakeFs *fs;
  Fs base;
  int fail_append, fail_barrier, unsynced;
  size_t durable_n;
  unsigned barriers;
} VisibleFs;

static int append(void *u, const char *path, const char *buf, size_t n) {
  VisibleFs *v = u;
  if (n == 0) {
    v->barriers++;
    if (v->fail_barrier) return EIO;
  }
  FFile *f = ffs_find(v->fs, path);
  size_t before = f ? f->n : 0;
  int rc = v->base.append_line(v->fs, path, buf, n);
  if (rc) return rc;
  if (v->fail_append && n) {
    if (!v->unsynced) v->durable_n = before;
    v->fail_append = 0;
    v->unsynced = 1;
    return EIO;
  }
  v->unsynced = 0; /* successful append or zero-byte barrier syncs this file */
  return 0;
}

static int tail(void *u, const char *p, char *b, size_t cap, size_t *n) {
  VisibleFs *v = u;
  return v->base.read_tail(v->fs, p, b, cap, n);
}

static int read_file(void *u, const char *p, void *b, size_t cap, size_t *n) {
  VisibleFs *v = u;
  return v->base.read_file(v->fs, p, b, cap, n);
}

static int write_file(void *u, const char *p, const void *b, size_t n) {
  VisibleFs *v = u;
  return v->base.write_file(v->fs, p, b, n);
}

static void boot_visible(Rig *r, VisibleFs *v) {
  Fs fs = {append, tail, write_file, read_file};
  CHECK_EQ(watcher_init(r->w, fp_iface(), r->pdm, fs, v, fenv_iface(), &r->env), 0);
}

static int32_t cursor(Rig *r) {
  PState st;
  uint32_t seq;
  int next;
  Fs fs = ffs_iface();
  CHECK_EQ(state_load(&fs, &r->fs, &st, &seq, &next), ST_OK);
  return st.cursor;
}

static void power_loss(Rig *r, VisibleFs *v) {
  if (v->unsynced) {
    FFile *f = ffs_find(&r->fs, AW_PATH_SESSIONS);
    if (f) f->n = v->durable_n;
    v->unsynced = 0;
  }
  rig_boot(r);
  rig_settle(r);
}

static void run_case(int restart, int barrier_fails, int power_before_retry) {
  Rig *r = rig_new();
  rig_settle(r);
  VisibleFs v = {.fs = &r->fs, .base = ffs_iface()};
  boot_visible(r, &v);
  rig_push(r, ev_app(0x0100000000001000ull, AE_IN_FOCUS, 1000));
  rig_settle(r);
  v.fail_append = 1;
  rig_at(r, 1010);
  rig_push(r, ev_app(0x0100000000001000ull, AE_OUT_OF_FOCUS, 1010));
  rig_poll(r);
  CHECK(v.unsynced);
  CHECK_EQ(cursor(r), 1);
  unsigned before = v.barriers;

  if (!power_before_retry) {
    v.fail_barrier = barrier_fails;
    if (restart) boot_visible(r, &v); /* same mounted filesystem/cache, new process */
    rig_poll(r);
    CHECK(v.barriers > before);
    if (barrier_fails) {
      CHECK(v.unsynced);
      CHECK_EQ(cursor(r), 1); /* successful state writes must not pass an unsynced key */
      CHECK_EQ(r->last_sleep >= 5000, 1);
      /* Let subsequent retry sync successfully in half the cases. Otherwise
       * lose the readable bytes now and require replay from the old cursor. */
      if (restart) {
        v.fail_barrier = 0;
        rig_settle(r);
        CHECK(!v.unsynced);
        CHECK_EQ(cursor(r), 2);
      }
    } else {
      CHECK(!v.unsynced);
      CHECK_EQ(cursor(r), 2);
    }
  }

  power_loss(r, &v);
  Journal_View j;
  jv_load(r, &j);
  CHECK_EQ(j.sessions, 1);
  CHECK_EQ(j.gaps, 1);
  CHECK_EQ(j.n_bad, 0);
  CHECK(jv_keys_increasing(&j));
  CHECK_EQ(cursor(r), 2);
  jv_free(&j);
  rig_del(r);
}

static void readable_unsynced_keys_require_a_journal_barrier(void) {
  for (int restart = 0; restart < 2; restart++)
    for (int fails = 0; fails < 2; fails++) run_case(restart, fails, 0);
  run_case(0, 0, 1);
}

int main(void) {
  RUN(readable_unsynced_keys_require_a_journal_barrier);
  return t_done("journal durability");
}
