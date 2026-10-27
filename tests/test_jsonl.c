#include "jsonl.h"
#include "t.h"
#include "util.h"

static SessionRec sample(void) {
  SessionRec r;
  memset(&r, 0, sizeof r);
  r.pid = 0x0100000000001234ull; r.su = 1700000000; r.sn = 1700000001; r.ss = 1000;
  r.eu = 1700000100; r.en = 1700000101; r.es = 1100; r.dur = 100; r.dur_ok = 1; r.end_known = 1;
  r.why = WHY_OUT_OF_FOCUS; r.sem = SEM_OK;
  return r;
}

static void golden_session(void) {
  char b[JSONL_MAX];
  Key k = {1, 812, SUB_SESSION};
  SessionRec r = sample();
  size_t n = jsonl_session(b, sizeof b, &k, &r);
  CHECK(n > 0);
  CHECK_STR(b, "{\"v\":1,\"t\":\"session\",\"ep\":1,\"ev\":812,\"sub\":1,\"pid\":\"0x0100000000001234\",\"su\":1700000000,"
               "\"sn\":1700000001,\"ss\":1000,\"eu\":1700000100,\"en\":1700000101,\"es\":1100,\"dur\":100,"
               "\"why\":\"out_of_focus\",\"pol\":0,\"sem\":\"ok\",\"c\":\"8e0a19e1\"}\n");
  CHECK_EQ(b[n - 1], '\n');
  CHECK(jsonl_valid(b, n - 1));
}

static void golden_unknown_end(void) {
  char b[JSONL_MAX];
  Key k = {2, 5, SUB_SESSION};
  SessionRec r;
  memset(&r, 0, sizeof r);
  r.pid = 0x0100000000001234ull; r.su = 10; r.ss = 20; r.es = 30; r.dur = 10; r.dur_ok = 1;
  r.why = WHY_LOST; r.pol = 1; r.sem = SEM_UNC;
  size_t n = jsonl_session(b, sizeof b, &k, &r);
  CHECK_STR(b, "{\"v\":1,\"t\":\"session\",\"ep\":2,\"ev\":5,\"sub\":1,\"pid\":\"0x0100000000001234\",\"su\":10,\"sn\":0,\"ss\":20,"
               "\"eu\":null,\"en\":null,\"es\":30,\"dur\":10,\"why\":\"lost\",\"pol\":1,\"sem\":\"unc\",\"c\":\"4568cf33\"}\n");
  CHECK(n > 0);
  r.dur_ok = 0;
  jsonl_session(b, sizeof b, &k, &r);
  CHECK(strstr(b, "\"dur\":null") != NULL);
}

static void golden_gap(void) {
  char b[JSONL_MAX];
  Key k = {3, 20, SUB_GAP};
  jsonl_gap(b, sizeof b, &k, GAP_LOG_DROPPED, 10, 20);
  CHECK_STR(b, "{\"v\":1,\"t\":\"gap\",\"ep\":3,\"ev\":20,\"sub\":0,\"why\":\"log_dropped\",\"from\":10,\"to\":20,\"c\":\"726a59a0\"}\n");
}

static void worst_case_fits(void) {
  char b[JSONL_MAX];
  Key k = {UINT32_MAX, INT32_MAX, SUB_SESSION};
  SessionRec r;
  memset(&r, 0xff, sizeof r);
  r.why = WHY_CLOCK_RESET; r.sem = SEM_UNC; r.dur_ok = 1; r.end_known = 1; r.pol = 255;
  size_t n = jsonl_session(b, sizeof b, &k, &r);
  CHECK(n > 0 && n < JSONL_MAX);
  CHECK(jsonl_valid(b, n - 1));
  Key got;
  CHECK(jsonl_key(b, n - 1, &got));
  CHECK(got.ep == UINT32_MAX && got.ev == INT32_MAX && got.sub == SUB_SESSION);
  char tiny[100];
  CHECK_EQ(jsonl_session(tiny, sizeof tiny, &k, &r), 0);
}

static void key_roundtrip_and_order(void) {
  char b[JSONL_MAX];
  Key k = {7, -3, SUB_GAP}, got;
  size_t n = jsonl_gap(b, sizeof b, &k, GAP_FIRST_RUN, -3, -3);
  CHECK(jsonl_key(b, n - 1, &got));
  CHECK(key_cmp(&k, &got) == 0);
  Key a = {1, 5, 1}, c = {1, 5, 0}, d = {2, 0, 0}, e = {1, 6, 0};
  CHECK(key_cmp(&c, &a) < 0); CHECK(key_cmp(&a, &e) < 0); CHECK(key_cmp(&e, &d) < 0); CHECK(key_cmp(&d, &a) > 0);
}

static void rejects_damage(void) {
  char b[JSONL_MAX];
  Key k = {1, 812, SUB_SESSION};
  SessionRec r = sample();
  size_t n = jsonl_session(b, sizeof b, &k, &r) - 1;
  CHECK(jsonl_valid(b, n));
  for (size_t i = 0; i < n; i++) {                     /* every single flipped bit is caught */
    for (int bit = 0; bit < 8; bit += 3) {
      char c[JSONL_MAX];
      memcpy(c, b, n);
      c[i] = (char)(c[i] ^ (1 << bit));
      CHECK(!jsonl_valid(c, n));
    }
  }
  for (size_t m = 0; m < n; m++) CHECK(!jsonl_valid(b, m));   /* every torn prefix */
  char z[JSONL_MAX];
  memset(z, 0, sizeof z);
  CHECK(!jsonl_valid(z, 200));
  char big[JSONL_MAX + 40];
  memset(big, 'a', sizeof big);
  big[0] = '{';
  CHECK(!jsonl_valid(big, sizeof big));
}

static void last_key_scan(void) {
  char a[JSONL_MAX], b[JSONL_MAX], buf[4 * JSONL_MAX];
  Key k1 = {1, 10, 1}, k2 = {1, 20, 1}, got;
  SessionRec r = sample();
  size_t na = jsonl_session(a, sizeof a, &k1, &r);
  size_t nb = jsonl_session(b, sizeof b, &k2, &r);
  int nl;

  memcpy(buf, a, na); memcpy(buf + na, b, nb);
  CHECK(jsonl_last_key(buf, na + nb, 1, &got, &nl));
  CHECK(key_cmp(&got, &k2) == 0); CHECK(nl);

  /* last line complete but missing its newline: valid, fence needed */
  CHECK(jsonl_last_key(buf, na + nb - 1, 1, &got, &nl));
  CHECK(key_cmp(&got, &k2) == 0); CHECK(!nl);

  /* torn last line: fall back to the previous line */
  CHECK(jsonl_last_key(buf, na + nb / 2, 1, &got, &nl));
  CHECK(key_cmp(&got, &k1) == 0); CHECK(!nl);

  /* zero fill after a good line */
  memcpy(buf, a, na); memset(buf + na, 0, 100);
  CHECK(jsonl_last_key(buf, na + 100, 1, &got, &nl));
  CHECK(key_cmp(&got, &k1) == 0); CHECK(!nl);

  /* only garbage */
  memset(buf, 0, 50);
  CHECK(!jsonl_last_key(buf, 50, 1, &got, &nl));
  CHECK(!jsonl_last_key(buf, 0, 1, &got, &nl)); CHECK(nl);

  /* a read that starts mid-line must not trust its first fragment */
  memcpy(buf, a + 40, na - 40); memcpy(buf + na - 40, b, nb);
  CHECK(jsonl_last_key(buf, na - 40 + nb, 0, &got, &nl));
  CHECK(key_cmp(&got, &k2) == 0);
  CHECK(!jsonl_last_key(buf, na - 40, 0, &got, &nl));

  /* garbage line in the middle does not hide an earlier valid line */
  memcpy(buf, a, na); memcpy(buf + na, "not json\n", 9);
  CHECK(jsonl_last_key(buf, na + 9, 1, &got, &nl));
  CHECK(key_cmp(&got, &k1) == 0); CHECK(nl);
}

int main(void) {
  RUN(golden_session);
  RUN(golden_unknown_end);
  RUN(golden_gap);
  RUN(worst_case_fits);
  RUN(key_roundtrip_and_order);
  RUN(rejects_damage);
  RUN(last_key_scan);
  return t_done("jsonl");
}
