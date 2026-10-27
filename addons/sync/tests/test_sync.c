#include "fakes.h"
#include "sync_priv.h"
#include "sha256.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define TOKEN "tok-AAAAAAAAAAAAAAAA"
#define DEV   "00000000-0000-0000-0000-000000000000"
#define SD    SYNC_DATA_DIR
#define CFG_GOOD "server=100.64.0.1:18766\ndevice=" DEV "\ntoken=" TOKEN "\n"

static Fake fk;
static SyncEngine eng;
static SyncCfg cfg;
static SyncPlat plat;
static const char *JP = SYNC_PATH_JOURNAL;

static void setup(void) {
  fk_init(&fk, TOKEN, DEV);
  CHECK(sync_cfg_parse(CFG_GOOD, strlen(CFG_GOOD), &cfg) == 0);
  fk_plat(&fk, &plat);
  sync_init(&eng, &plat, &cfg);
}
static void restart(void) { sync_init(&eng, &plat, &cfg); }
static void teardown(void) {
  CHECK(fk_writes_clean(&fk, cfg.state_dir, cfg.journal));
  CHECK(fk.net_open_n == fk.net_close_n);
  fk_free(&fk);
}

static size_t mkline(char *buf, size_t cap, int ep, int ev, int sub, int dur) {
  char body[300];
  int n = snprintf(body, sizeof body, "{\"v\":1,\"t\":\"session\",\"ep\":%d,\"ev\":%d,\"sub\":%d,\"pid\":\"0x0100000000001000\",\"su\":1700000000,\"sn\":1,\"ss\":1000,\"eu\":1,\"en\":1,\"es\":1100,\"dur\":%d,\"why\":\"exit\",\"pol\":0,\"sem\":\"ok\"", ep, ev, sub, dur);
  return (size_t)snprintf(buf, cap, "%.*s,\"c\":\"%08x\"}", n, body, crc32_of(body, (size_t)n));
}

static size_t jsize(void) { size_t n = 0; fk_get(&fk, JP, &n); return n; }
/* appends a valid line plus newline, returns the end offset */
static size_t add(int ep, int ev) {
  char l[400];
  size_t n = mkline(l, sizeof l, ep, ev, 1, 100);
  l[n] = '\n';
  fk_append(&fk, JP, l, n + 1);
  return jsize();
}
static void adds(const char *s) { fk_append(&fk, JP, s, strlen(s)); }
static SyncRun go(void) { return sync_run_once(&eng); }
static void bad_crc(char *l) { char *p = strstr(l, "\"c\":\"") + 5; *p = *p == '0' ? '1' : '0'; }

static void test_sha(void) {
  uint8_t d[32]; char h[65]; static char big[1000000]; Sha256 s;
  sha256_of("", 0, d); sha256_hex(d, h);
  CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  sha256_of("abc", 3, d); sha256_hex(d, h);
  CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  sha256_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d); sha256_hex(d, h);
  CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  memset(big, 'a', sizeof big);
  sha256_of(big, sizeof big, d); sha256_hex(d, h);
  CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  sha256_init(&s);
  for (size_t i = 0; i < sizeof big; i += 77) sha256_update(&s, big + i, sizeof big - i < 77 ? sizeof big - i : 77);
  sha256_final(&s, d); sha256_hex(d, h);
  CHECK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
  CHECK(crc32_of("123456789", 9) == 0xcbf43926u);
}

static int cfg_try(const char *t) { SyncCfg c; return sync_cfg_parse(t, strlen(t), &c); }
static void test_cfg(void) {
  SyncCfg c;
  const char *dev = "device=" DEV "\n", *tok = "token=" TOKEN "\n";
  char b[700];
  CHECK(cfg_try(CFG_GOOD) == 0);
  CHECK(sync_cfg_parse(CFG_GOOD, strlen(CFG_GOOD), &c) == 0 && c.ip[0] == 100 && c.ip[1] == 64 && c.port == 18766 &&
        !strcmp(c.journal, SYNC_PATH_JOURNAL) && !strcmp(c.state_dir, SYNC_DATA_DIR));
  CHECK(cfg_try("server=100.64.0.1:18766\ndevice=" DEV "\ntoken=" TOKEN) == 0);
  CHECK(cfg_try(CFG_GOOD "journal=/config/aw-watcher-switch-sync/test.jsonl\nstate_dir=/config/aw-watcher-switch-sync/test\n") == 0);
  const char *bad_servers[] = {"gerry:18766", "0.0.0.0:18766", "192.168.1.5:18766", "100.63.255.255:1", "100.128.0.0:1", "100.64.0.1:0",
    "100.64.0.1:65536", "100.064.0.1:1", "100.64.0.1", "100.64.0.1:", "100.64.0.256:1", "100.64.0.1:1 ", "100.64.0.1:80x", "[::1]:80", "127.0.0.1:80", NULL};
  for (int i = 0; bad_servers[i]; i++) {
    snprintf(b, sizeof b, "server=%s\n%s%s", bad_servers[i], dev, tok);
    CHECK(cfg_try(b) == -EINVAL);
  }
  CHECK(cfg_try("server=100.127.255.255:1\n" "device=" DEV "\n" "token=" TOKEN "\n") == 0);
  CHECK(cfg_try("server=100.64.0.0:1\n" "device=" DEV "\n" "token=" TOKEN "\n") == 0);
  CHECK(cfg_try(CFG_GOOD "token=" TOKEN "\n") == -EINVAL);
  CHECK(cfg_try(CFG_GOOD "bogus=1\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\r\n" "device=" DEV "\n" "token=" TOKEN "\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\ndevice=" DEV "\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\ntoken=" TOKEN "\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\ndevice=0000000A-0000-0000-0000-000000000000\ntoken=" TOKEN "\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\ndevice=00000000-0000-0000-0000-00000000000\ntoken=" TOKEN "\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\ndevice=" DEV "\ntoken=short\n") == -EINVAL);
  CHECK(cfg_try("server=100.64.0.1:1\ndevice=" DEV "\ntoken=aaaaaaaaaaaaaaaa bbbb\n") == -EINVAL);
  CHECK(cfg_try(CFG_GOOD "\n") == -EINVAL);
  memset(b, 'x', sizeof b); memcpy(b, CFG_GOOD, strlen(CFG_GOOD));
  CHECK(sync_cfg_parse(b, 513, &c) == -EINVAL);
  CHECK(sync_cfg_parse("server=100.64.0.1:1\n\0device", 27, &c) == -EINVAL);
  /* state_dir and journal path rules */
  const char *bad_paths[] = {
    "state_dir=/config/aw-watcher-switch", "state_dir=/config/aw-watcher-switch/x", "state_dir=/CONFIG/AW-Watcher-Switch/x",
    "state_dir=/config/aw-watcher-switch-sync/../aw-watcher-switch/x", "state_dir=relative/dir", "state_dir=/config//x",
    "state_dir=/config/x/", "state_dir=/config/./x", "state_dir=/config/x y", "state_dir=",
    "journal=/config/aw-watcher-switch-sync/../x.jsonl", "journal=sessions.jsonl", "journal=/sessions.jsonl",
    "journal=/data/w/j.jsonl\nstate_dir=/data/w", "journal=/data/w/s/j.jsonl\nstate_dir=/data/w/s", NULL};
  for (int i = 0; bad_paths[i]; i++) {
    snprintf(b, sizeof b, "%s%s\n", CFG_GOOD, bad_paths[i]);
    CHECK(cfg_try(b) == -EINVAL);
  }
  CHECK(cfg_try(CFG_GOOD "state_dir=/config/aw-watcher-switch-sync2\n") == 0);
  CHECK(cfg_try(CFG_GOOD "state_dir=/config\n") == -EINVAL);
  CHECK(cfg_try(CFG_GOOD "journal=/data/w/j.jsonl\nstate_dir=/data/w/s\n") == 0);
}

static void test_good_upload(void) {
  size_t n; const uint8_t *st;
  setup();
  for (int i = 1; i <= 5; i++) add(1, i);
  SyncRun r = go();
  CHECK(r.outcome == SYNC_IDLE && r.delay_ms == SYNC_IDLE_MS);
  CHECK(fk_nrecs(&fk) == 5 && fk.exch_n == 1);
  CHECK(eng.ck.offset == jsize() && eng.ck.acked == 5);
  r = go();
  CHECK(r.outcome == SYNC_IDLE && fk.exch_n == 1 && fk.net_open_n == 1);
  st = fk_get(&fk, SD "/" SYNC_NAME_STATUS, &n);
  CHECK(st && n == 320 && st[319] == '\n' && !memcmp(st, "{\"stage\":", 9));
  CHECK(strstr((const char *)st, "\"result\":\"idle\"") != NULL);
  teardown();
}

static void test_batches(void) {
  setup();
  for (int i = 1; i <= 100; i++) add(1, i);
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(fk.exch_n == 4 && fk_nrecs(&fk) == 100 && eng.ck.acked == 100 && fk.net_open_n == 1);
  CHECK(strcmp(fk.sent_ids[0], fk.sent_ids[1]) != 0);
  CHECK(strlen(fk.sent_ids[0]) > 16 && fk.sent_ids[0][16] == '-' && strstr(fk.sent_ids[0], "-0-32") && !strcmp(fk.sent_ids[3] + strlen(fk.sent_ids[3]) - 2, "-4"));
  teardown();
  setup();
  for (int i = 1; i <= 1400; i++) add(1, i);
  SyncRun r = go();
  CHECK(r.outcome == SYNC_MORE && fk.exch_n == SYNC_MAX_BATCHES_PER_RUN && fk.net_open_n == 1 && fk.net_close_n == 1);
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 1400);
  teardown();
}

static void test_lost_ack(void) {
  uint8_t s1[32];
  setup();
  for (int i = 1; i <= 5; i++) add(1, i);
  fk.mode = M_DROP_RESP; fk.mode_left = 1;
  SyncRun r = go();
  CHECK(r.outcome == SYNC_RETRY && r.delay_ms == SYNC_BACKOFF_MIN_MS);
  CHECK(eng.ck.offset == 0 && fk_nrecs(&fk) == 5);
  memcpy(s1, fk.last_body_sha, 32);
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(!strcmp(fk.sent_ids[0], fk.sent_ids[1]) && !memcmp(s1, fk.last_body_sha, 32));
  CHECK(eng.ck.offset == jsize() && fk_nrecs(&fk) == 5 && fk.exch_n == 2);
  teardown();
}

static void test_bad_acks(void) {
  static const FkMode retry_modes[] = {M_DROP_RESP, M_WRONG_ID, M_WRONG_SHA, M_WRONG_COUNT, M_BAD_SUM, M_S500, M_TRUNC, M_BIG_CL, M_EXTRA, M_OLD_ACK, M_NO_CL, M_CHUNKED, M_GARBAGE};
  static const FkMode block_modes[] = {M_S400, M_S401, M_S403, M_S409, M_S413};
  for (size_t i = 0; i < sizeof retry_modes / sizeof *retry_modes; i++) {
    setup();
    for (int j = 1; j <= 3; j++) add(1, j);
    CHECK(go().outcome == SYNC_IDLE);
    size_t base = eng.ck.offset;
    add(1, 10); add(1, 11);
    fk.mode = retry_modes[i]; fk.mode_left = 1;
    SyncRun r = go();
    CHECK(r.outcome == SYNC_RETRY);
    CHECK(eng.ck.offset == base && eng.ck.acked == 3);
    CHECK(go().outcome == SYNC_IDLE && eng.ck.offset == jsize());
    teardown();
  }
  for (size_t i = 0; i < sizeof block_modes / sizeof *block_modes; i++) {
    setup();
    add(1, 1);
    fk.mode = block_modes[i]; fk.mode_left = 1;
    SyncRun r = go();
    CHECK(r.outcome == SYNC_BLOCKED && r.delay_ms == SYNC_BLOCKED_MS && eng.ck.offset == 0);
    if (block_modes[i] == M_S409) CHECK(!strcmp(eng.conflict, "1/2/1") && eng.last_http == 409);
    CHECK(go().outcome == SYNC_IDLE && eng.ck.offset == jsize());
    teardown();
  }
}

static void test_backoff(void) {
  uint32_t want[] = {5000, 10000, 20000, 40000, 80000, 160000, 320000, 600000, 600000};
  setup();
  add(1, 1);
  fk.mode = M_S500; fk.mode_left = -1;
  for (size_t i = 0; i < sizeof want / sizeof *want; i++) { SyncRun r = go(); CHECK(r.outcome == SYNC_RETRY && r.delay_ms == want[i]); }
  fk.mode = M_OK;
  CHECK(go().outcome == SYNC_IDLE && eng.attempts == 0);
  add(1, 2);
  fk.mode = M_S500; fk.mode_left = 1;
  CHECK(go().delay_ms == SYNC_BACKOFF_MIN_MS);
  teardown();
  setup();
  add(1, 1);
  fk.net_fail = ENETDOWN;
  CHECK(go().outcome == SYNC_RETRY && fk.exch_n == 0 && fk.net_close_n == 0);
  fk.net_fail = 0;
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 1);
  teardown();
}

static void grow_hook(Fake *f, int when, int idx) {
  if (when == 1 && idx == 0) { char l[400]; for (int i = 100; i < 103; i++) { size_t n = mkline(l, sizeof l, 1, i, 1, 100); l[n] = '\n'; fk_append(f, SYNC_PATH_JOURNAL, l, n + 1); } }
}
static void test_overlap(void) {
  setup();
  for (int i = 1; i <= 40; i++) add(1, i);
  fk.hook = grow_hook;
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(fk_nrecs(&fk) == 43 && eng.ck.offset == jsize() && fk.exch_n == 2);
  teardown();
  /* lost checkpoint: full replay, server dedups */
  setup();
  for (int i = 1; i <= 70; i++) add(1, i);
  CHECK(go().outcome == SYNC_IDLE);
  fk_put(&fk, SD "/sync.a", "garbage", 7); fk_put(&fk, SD "/sync.b", "garbage", 7);
  fk.nbatches = 0;
  restart();
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(fk_nrecs(&fk) == 70 && fk.last_new == 0 && fk.last_dup > 0 && eng.ck.offset == jsize());
  /* overlapping range with a different batch id: exactly the duplicated records are dup */
  teardown();
}

static void test_ckpt_sweep(void) {
  uint8_t keep[2][128];
  size_t n;
  setup();
  for (int i = 1; i <= 70; i++) add(1, i);
  CHECK(go().outcome == SYNC_IDLE);
  size_t total = jsize();
  for (int s = 0; s < 2; s++) {
    const uint8_t *d = fk_get(&fk, s ? SD "/sync.b" : SD "/sync.a", &n);
    CHECK(d && n == 128);
    memcpy(keep[s], d, 128);
  }
  for (int s = 0; s < 3; s++) {
    for (int byte = 0; byte < 128; byte++) {
      uint8_t x[128];
      fk_put(&fk, SD "/sync.a", keep[0], 128); fk_put(&fk, SD "/sync.b", keep[1], 128);
      memcpy(x, keep[s == 2 ? 0 : s], 128);
      x[byte] ^= 0x40;
      fk_put(&fk, s == 1 ? SD "/sync.b" : SD "/sync.a", x, 128);
      if (s == 2) fk_put(&fk, SD "/sync.b", x, 128);
      restart();
      SyncRun r = go();
      CHECK(r.outcome == SYNC_IDLE && fk_nrecs(&fk) == 70 && eng.ck.offset == total);
      CHECK(fk.net_open_n == fk.net_close_n);
    }
  }
  /* a seq tie or stale slot never points past the file */
  fk_put(&fk, SD "/sync.a", keep[0], 128); fk_del(&fk, SD "/sync.b");
  restart();
  CHECK(go().outcome == SYNC_IDLE && eng.ck.offset == total);
  teardown();
}

static void test_partial(void) {
  char l[400]; size_t n;
  setup();
  size_t e1 = add(1, 1);
  n = mkline(l, sizeof l, 1, 2, 1, 100);
  fk_append(&fk, JP, l, 50);
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(fk_nrecs(&fk) == 1 && eng.ck.offset == e1 && eng.held_bytes == 50);
  { size_t sn; const uint8_t *st = fk_get(&fk, SD "/" SYNC_NAME_STATUS, &sn); CHECK(strstr((const char *)st, "\"held_bytes\":50") != NULL); }
  CHECK(go().outcome == SYNC_IDLE && fk.exch_n == 1);
  fk_append(&fk, JP, l + 50, n - 50); fk_append(&fk, JP, "\n", 1);
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 2 && eng.held_bytes == 0 && eng.ck.offset == jsize());
  teardown();
  /* CRC-valid last line without newline: sent normalized, cursor over the original bytes, later fence append */
  setup();
  n = mkline(l, sizeof l, 1, 1, 1, 100); fk_append(&fk, JP, l, n); fk_append(&fk, JP, "\n", 1);
  n = mkline(l, sizeof l, 1, 2, 1, 100); fk_append(&fk, JP, l, n);
  size_t total = jsize();
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 2 && eng.ck.offset == total && eng.held_bytes == 0);
  CHECK(strlen(fk.recs[1].line) == n && fk.recs[1].line[n - 1] == '}');
  n = mkline(l, sizeof l, 1, 3, 1, 100);
  fk_append(&fk, JP, "\n", 1); fk_append(&fk, JP, l, n); fk_append(&fk, JP, "\n", 1);
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 3 && fk.last_new == 1 && fk.last_dup == 0 && eng.ck.offset == jsize());
  restart();
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 3 && fk.exch_n == 2);
  teardown();
  /* an unterminated tail that outgrows the line limit is held, then skipped once terminated */
  setup();
  add(1, 1);
  { char junk[2000]; memset(junk, 'x', sizeof junk); fk_append(&fk, JP, junk, sizeof junk);
    CHECK(go().outcome == SYNC_IDLE && eng.held_bytes == 2000 && fk_nrecs(&fk) == 1);
    fk_append(&fk, JP, "\n", 1); add(1, 2);
    CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 2 && eng.held_bytes == 0 && eng.skipped_lines == 1 && eng.skipped_bytes == 2001); }
  teardown();
}

static void test_garbage(void) {
  char l[400], big[3000];
  size_t n;
  setup();
  add(1, 1);
  n = mkline(l, sizeof l, 1, 90, 1, 100); bad_crc(l); l[n] = '\n'; fk_append(&fk, JP, l, n + 1);
  adds("{\"v\":2,\"t\":\"session\"}\n");
  adds("\n");
  memset(big, 'x', sizeof big); big[sizeof big - 1] = '\n'; fk_append(&fk, JP, big, sizeof big);
  n = mkline(l, sizeof l, 1, 91, 1, 100); l[n] = '\r'; l[n + 1] = '\n'; fk_append(&fk, JP, l, n + 2);
  size_t e2 = add(1, 2);
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(fk_nrecs(&fk) == 2 && fk_has(&fk, 1, 1, 1) && fk_has(&fk, 1, 2, 1) && !fk_has(&fk, 1, 90, 1) && !fk_has(&fk, 1, 91, 1));
  CHECK(eng.skipped_lines == 4 && eng.ck.offset == e2);
  /* trailing garbage is counted but never acked past */
  adds("garbage line\n");
  CHECK(go().outcome == SYNC_IDLE && eng.ck.offset == e2 && eng.trail_lines == 1 && eng.skipped_lines == 4);
  { size_t sn; const uint8_t *st = fk_get(&fk, SD "/" SYNC_NAME_STATUS, &sn); CHECK(strstr((const char *)st, "\"skipped_lines\":5") != NULL); }
  add(1, 3);
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 3 && eng.skipped_lines == 5);
  teardown();
}

static void test_anchor(void) {
  size_t n; char *copy;
  /* different replacement */
  setup();
  for (int i = 1; i <= 10; i++) add(1, i);
  CHECK(go().outcome == SYNC_IDLE);
  fk_del(&fk, JP);
  for (int i = 1; i <= 3; i++) add(2, i);
  CHECK(go().outcome == SYNC_IDLE);
  CHECK(eng.resets == 1 && fk_nrecs(&fk) == 13 && eng.ck.offset == jsize());
  teardown();
  /* truncated prefix of the same data: replayed from 0, all dup */
  setup();
  for (int i = 1; i <= 10; i++) add(1, i);
  CHECK(go().outcome == SYNC_IDLE);
  const uint8_t *d = fk_get(&fk, JP, &n);
  copy = malloc(n); memcpy(copy, d, n);
  size_t keep = n / 2;
  fk_put(&fk, JP, copy, keep);
  CHECK(go().outcome == SYNC_IDLE && eng.resets == 1 && fk_nrecs(&fk) == 10);
  /* same length rewrite inside the tail region */
  fk_put(&fk, JP, copy, n);
  CHECK(go().outcome == SYNC_IDLE);
  size_t resets = eng.resets;
  copy[n - 20] ^= 1;
  fk_put(&fk, JP, copy, n);
  SyncRun r = go();
  CHECK(r.outcome == SYNC_IDLE && eng.resets == resets + 1);
  /* same length rewrite inside the anchor region, then a longer file with a different head */
  copy[10] ^= 1; fk_put(&fk, JP, copy, n);
  resets = eng.resets;
  go();
  CHECK(eng.resets == resets + 1);
  free(copy);
  fk_del(&fk, JP);
  add(7, 1); add(7, 2);
  for (int i = 3; i < 30; i++) add(7, i);
  resets = eng.resets;
  go();
  CHECK(eng.resets == resets + 1 && fk_has(&fk, 7, 29, 1));
  teardown();
  /* missing journal: idle, checkpoint untouched, resumes later */
  setup();
  for (int i = 1; i <= 3; i++) add(1, i);
  go();
  size_t off = eng.ck.offset; uint32_t seq = eng.ck.seq;
  n = jsize();
  d = fk_get(&fk, JP, &n);
  copy = malloc(n); memcpy(copy, d, n);
  fk_del(&fk, JP);
  r = go();
  CHECK(r.outcome == SYNC_IDLE && !strcmp(eng.result, "no_journal") && eng.ck.offset == off && eng.ck.seq == seq && eng.resets == 0);
  fk_put(&fk, JP, copy, n); free(copy);
  add(1, 4);
  CHECK(go().outcome == SYNC_IDLE && eng.resets == 0 && fk_nrecs(&fk) == 4);
  teardown();
}

static void test_read_errors(void) {
  setup();
  for (int i = 1; i <= 3; i++) add(1, i);
  go();
  size_t off = eng.ck.offset;
  add(1, 4);
  fk.read_fail_after = fk.journal_reads; fk.journal_read_err = EIO;
  SyncRun r = go();
  CHECK(r.outcome == SYNC_RETRY && eng.ck.offset == off && eng.resets == 0 && strcmp(eng.result, "no_journal") != 0);
  fk.journal_read_err = ENOENT;
  r = go();
  CHECK(r.outcome == SYNC_RETRY && eng.ck.offset == off && eng.resets == 0 && strcmp(eng.result, "no_journal") != 0);
  fk.read_fail_after = 0;
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 4 && eng.resets == 0);
  teardown();
  /* file_size failure other than ENOENT */
  setup();
  add(1, 1);
  fk.fail_at = fk.calls + 1; fk.fail_err = EIO;
  CHECK(go().outcome == SYNC_RETRY);
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 1);
  teardown();
}

static uint8_t *shared_copy; static size_t shared_n;
static void rewrite_all_hook(Fake *f, int when, int idx) {
  if (when == 0 && idx == 0) { uint8_t *c = malloc(shared_n); memcpy(c, shared_copy, shared_n); c[shared_n / 2] ^= 1; c[5] ^= 1; c[shared_n - 3] ^= 1; fk_put(f, SYNC_PATH_JOURNAL, c, shared_n); free(c); }
}
static void rewrite_tail_hook(Fake *f, int when, int idx) {
  if (when == 1 && idx == 0) { uint8_t *c = malloc(shared_n); memcpy(c, shared_copy, shared_n); c[shared_n - 5] ^= 1; fk_put(f, SYNC_PATH_JOURNAL, c, shared_n); free(c); }
}
static void truncate_hook(Fake *f, int when, int idx) {
  if (when == 1 && idx == 0) fk_put(f, SYNC_PATH_JOURNAL, shared_copy, shared_n / 3);
}
static void append_hook(Fake *f, int when, int idx) {
  if (when == 0 && idx == 0) { char l[400]; size_t n = mkline(l, sizeof l, 1, 500, 1, 1); l[n] = '\n'; fk_append(f, SYNC_PATH_JOURNAL, l, n + 1); }
}

static void snap(void) { size_t n; const uint8_t *d = fk_get(&fk, JP, &n); free(shared_copy); shared_copy = malloc(n); memcpy(shared_copy, d, n); shared_n = n; }

static void test_races(void) {
  /* journal replaced between scan and ack: the scanned offset must not be bound to the new file */
  setup();
  for (int i = 1; i <= 5; i++) add(1, i);
  snap();
  fk.hook = rewrite_all_hook;
  go();
  CHECK(eng.resets >= 1);
  CHECK(fk_has(&fk, 1, 1, 1));
  {
    size_t resets = eng.resets, off = eng.ck.offset;
    fk.hook = NULL;
    CHECK(go().outcome == SYNC_IDLE);
    CHECK(eng.resets == resets && eng.ck.offset == off && off > 0 && off <= jsize());
  }
  teardown();
  /* only the tail changes after the commit */
  setup();
  for (int i = 1; i <= 5; i++) add(1, i);
  snap();
  fk.hook = rewrite_tail_hook;
  fk.mode = M_OK;
  SyncRun r = go();
  CHECK(r.outcome != SYNC_RETRY && eng.resets >= 1);
  teardown();
  /* truncation during the exchange */
  setup();
  for (int i = 1; i <= 40; i++) add(1, i);
  snap();
  fk.hook = truncate_hook;
  go();
  CHECK(eng.resets >= 1);
  teardown();
  /* the journal grows during the exchange: cursor stops at the scanned bytes */
  setup();
  for (int i = 1; i <= 5; i++) add(1, i);
  size_t end5 = jsize();
  fk.hook = append_hook;
  r = go();
  CHECK(r.outcome == SYNC_IDLE && fk.exch_n == 2 && fk_nrecs(&fk) == 6 && eng.resets == 0 && eng.ck.offset == jsize() && end5 < jsize());
  teardown();
  free(shared_copy); shared_copy = NULL;
}

static void test_conflict(void) {
  char l[400]; size_t n;
  setup();
  add(1, 5);
  CHECK(go().outcome == SYNC_IDLE);
  size_t off = eng.ck.offset;
  n = mkline(l, sizeof l, 1, 5, 1, 999); l[n] = '\n'; fk_append(&fk, JP, l, n + 1);
  SyncRun r = go();
  CHECK(r.outcome == SYNC_BLOCKED && eng.last_http == 409 && !strcmp(eng.conflict, "1/5/1") && eng.ck.offset == off && fk_nrecs(&fk) == 1);
  { size_t sn; const uint8_t *st = fk_get(&fk, SD "/" SYNC_NAME_STATUS, &sn); CHECK(strstr((const char *)st, "\"result\":\"blocked\"") && strstr((const char *)st, "\"conflict\":\"1/5/1\"")); }
  teardown();
}

static void test_writes_and_cfg(void) {
  SyncCfg bad;
  setup();
  add(1, 1);
  bad = cfg;
  strcpy(bad.state_dir, "/config/aw-watcher-switch/x");
  sync_init(&eng, &plat, &bad);
  SyncRun r = go();
  CHECK(r.outcome == SYNC_BLOCKED && fk.nw == 0 && fk.exch_n == 0 && fk.net_open_n == 0);
  bad = cfg;
  strcpy(bad.state_dir, "/config/../etc");
  sync_init(&eng, &plat, &bad);
  CHECK(go().outcome == SYNC_BLOCKED && fk.nw == 0);
  bad = cfg;
  bad.ip[0] = 192;
  sync_init(&eng, &plat, &bad);
  CHECK(go().outcome == SYNC_BLOCKED && fk.exch_n == 0);
  sync_init(&eng, &plat, &cfg);
  CHECK(go().outcome == SYNC_IDLE && fk.nw > 0);
  for (size_t i = 0; i < fk.nw; i++) CHECK(!strncmp(fk.wlog[i], SD "/", strlen(SD) + 1));
  /* the journal bytes are untouched */
  teardown();
}

static void test_deadline(void) {
  setup();
  for (int i = 1; i <= 100; i++) add(1, i);
  fk.exch_advance = SYNC_RUN_TIMEOUT_MS + 10;
  SyncRun r = go();
  CHECK(r.outcome == SYNC_MORE && fk.exch_n == 1 && fk_nrecs(&fk) == 32);
  fk.exch_advance = 0;
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 100);
  teardown();
  /* expired before any exchange */
  setup();
  add(1, 1);
  fk.exch_advance = 0;
  fk.now = 5;
  CHECK(go().outcome == SYNC_IDLE);
  teardown();
}

static void test_ckpt_write_failure(void) {
  setup();
  for (int i = 1; i <= 70; i++) add(1, i);
  fk.fail_write_prefix = 1;
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 70 && eng.ck.offset == jsize());
  restart();
  fk.fail_write_prefix = 0;
  CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 70 && eng.ck.offset == jsize());
  teardown();
}

/* Fault sweep: inject an error at every failable platform call of a multi-batch run. */
static size_t ends[80]; static int nvalid;
static void build_sweep_journal(void) {
  char l[400];
  nvalid = 0;
  for (int i = 1; i <= 70; i++) {
    if (i == 5) { size_t n = mkline(l, sizeof l, 1, 900, 1, 1); bad_crc(l); l[n] = '\n'; fk_append(&fk, JP, l, n + 1); }
    if (i == 20) adds("\n");
    ends[nvalid++] = add(1, i);
  }
}
static void sweep_invariant(void) {
  CHECK(eng.ck.offset <= jsize());
  for (int i = 0; i < nvalid; i++) if (ends[i] <= eng.ck.offset) CHECK(fk_has(&fk, 1, i + 1, 1));
  CHECK(fk_nrecs(&fk) <= 70);
}
static void test_fault_sweep(void) {
  int total;
  setup();
  build_sweep_journal();
  CHECK(go().outcome == SYNC_IDLE);
  total = fk.calls;
  teardown();
  CHECK(total > 10);
  for (int err = 0; err < 2; err++) {
    for (int k = 1; k <= total + 2; k++) {
      setup();
      build_sweep_journal();
      fk.fail_at = k; fk.fail_err = err ? ETIMEDOUT : EIO;
      for (int run = 0; run < 12; run++) {
        SyncRun r = go();
        sweep_invariant();
        CHECK(r.outcome != SYNC_BLOCKED);
        if (run >= 2 && r.outcome == SYNC_IDLE) break;
      }
      CHECK(fk_nrecs(&fk) == 70 && eng.ck.offset == jsize());
      CHECK(go().outcome == SYNC_IDLE);
      teardown();
    }
  }
  /* persistent write failure and lost responses at every exchange */
  for (int k = 0; k < 4; k++) {
    setup();
    build_sweep_journal();
    fk.mode = M_DROP_RESP; fk.mode_left = -1;
    for (int run = 0; run <= k; run++) { CHECK(go().outcome == SYNC_RETRY); sweep_invariant(); }
    fk.mode_left = 0;
    CHECK(go().outcome == SYNC_IDLE && fk_nrecs(&fk) == 70 && eng.ck.offset == jsize());
    teardown();
  }
}

int main(void) {
  test_sha();
  test_cfg();
  test_good_upload();
  test_batches();
  test_lost_ack();
  test_bad_acks();
  test_backoff();
  test_overlap();
  test_ckpt_sweep();
  test_partial();
  test_garbage();
  test_anchor();
  test_read_errors();
  test_races();
  test_conflict();
  test_writes_and_cfg();
  test_deadline();
  test_ckpt_write_failure();
  test_fault_sweep();
  if (fails) { printf("%d of %d checks FAILED\n", fails, checks); return 1; }
  printf("%d checks\nALL TESTS PASSED\n", checks);
  return 0;
}
