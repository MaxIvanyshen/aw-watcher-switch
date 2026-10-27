#include "jsonl.h"

#include <string.h>

#include "util.h"

static const char CRC_TAG[] = ",\"c\":\"";
#define CRC_TAG_LEN 6
#define CRC_SUFFIX_LEN (CRC_TAG_LEN + 8 + 2) /* ,"c":"xxxxxxxx"} */

int key_cmp(const Key *a, const Key *b) {
  if (a->ep != b->ep) return a->ep < b->ep ? -1 : 1;
  if (a->ev != b->ev) return a->ev < b->ev ? -1 : 1;
  if (a->sub != b->sub) return a->sub < b->sub ? -1 : 1;
  return 0;
}

const char *gap_why_name(unsigned why) {
  static const char *n[GAP_COUNT] = {"none", "first_run", "state_lost", "state_corrupt", "log_dropped", "log_reset", "steady_reset"};
  return why < GAP_COUNT ? n[why] : "?";
}

static size_t seal(SBuf *s) {
  uint32_t crc = crc32_of(s->p, s->len);
  sb_str(s, CRC_TAG);
  sb_hex(s, crc, 8);
  sb_str(s, "\"}\n");
  return s->ovf ? 0 : s->len;
}

static void key_fields(SBuf *s, const Key *k) {
  sb_str(s, ",\"ep\":"); sb_u64(s, k->ep);
  sb_str(s, ",\"ev\":"); sb_i64(s, k->ev);
  sb_str(s, ",\"sub\":"); sb_u64(s, k->sub);
}

static void field_u64(SBuf *s, const char *name, uint64_t v) {
  sb_ch(s, ','); sb_ch(s, '"'); sb_str(s, name); sb_str(s, "\":"); sb_u64(s, v);
}

static void field_u64_or_null(SBuf *s, const char *name, uint64_t v, int known) {
  if (known) { field_u64(s, name, v); return; }
  sb_ch(s, ','); sb_ch(s, '"'); sb_str(s, name); sb_str(s, "\":null");
}

size_t jsonl_session(char *buf, size_t cap, const Key *k, const SessionRec *r) {
  SBuf s;
  sb_init(&s, buf, cap);
  sb_str(&s, "{\"v\":1,\"t\":\"session\"");
  key_fields(&s, k);
  sb_str(&s, ",\"pid\":\"0x"); sb_hex(&s, r->pid, 16); sb_ch(&s, '"');
  field_u64(&s, "su", r->su); field_u64(&s, "sn", r->sn); field_u64(&s, "ss", r->ss);
  field_u64_or_null(&s, "eu", r->eu, r->end_known);
  field_u64_or_null(&s, "en", r->en, r->end_known);
  field_u64(&s, "es", r->es);
  field_u64_or_null(&s, "dur", r->dur, r->dur_ok);
  sb_str(&s, ",\"why\":\""); sb_str(&s, close_why_name(r->why)); sb_ch(&s, '"');
  field_u64(&s, "pol", r->pol);
  sb_str(&s, ",\"sem\":\""); sb_str(&s, sem_name(r->sem)); sb_ch(&s, '"');
  return seal(&s);
}

size_t jsonl_gap(char *buf, size_t cap, const Key *k, GapWhy why, int32_t from, int32_t to) {
  SBuf s;
  sb_init(&s, buf, cap);
  sb_str(&s, "{\"v\":1,\"t\":\"gap\"");
  key_fields(&s, k);
  sb_str(&s, ",\"why\":\""); sb_str(&s, gap_why_name(why)); sb_ch(&s, '"');
  sb_str(&s, ",\"from\":"); sb_i64(&s, from);
  sb_str(&s, ",\"to\":"); sb_i64(&s, to);
  return seal(&s);
}

static int hex8(const char *p, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 8; i++) {
    char c = p[i];
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) return 0;
    v = v << 4 | (uint32_t)d;
  }
  *out = v;
  return 1;
}

int jsonl_valid(const char *line, size_t n) {
  if (n < CRC_SUFFIX_LEN + 2 || n >= JSONL_MAX || line[0] != '{') return 0;
  size_t body = n - CRC_SUFFIX_LEN;
  uint32_t want;
  if (memcmp(line + body, CRC_TAG, CRC_TAG_LEN) != 0) return 0;
  if (line[n - 2] != '"' || line[n - 1] != '}') return 0;
  if (!hex8(line + body + CRC_TAG_LEN, &want)) return 0;
  return crc32_of(line, body) == want;
}

static int num(const char *p, const char *end, const char *tag, int64_t *v, const char **next) {
  size_t tl = strlen(tag);
  if ((size_t)(end - p) < tl || memcmp(p, tag, tl) != 0) return 0;
  p += tl;
  int neg = 0, any = 0;
  if (p < end && *p == '-') { neg = 1; p++; }
  int64_t x = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    if (x > (INT64_MAX - 9) / 10) return 0;
    x = x * 10 + (*p - '0'); p++; any = 1;
  }
  if (!any) return 0;
  *v = neg ? -x : x;
  *next = p;
  return 1;
}

int jsonl_key(const char *line, size_t n, Key *k) {
  if (!jsonl_valid(line, n)) return 0;
  const char *end = line + n;
  const char *p = line;
  int64_t ep, ev, sub;
  while (p + 5 <= end && memcmp(p, "\"ep\":", 5) != 0) p++;
  if (p + 5 > end) return 0;
  if (!num(p, end, "\"ep\":", &ep, &p)) return 0;
  if (!num(p, end, ",\"ev\":", &ev, &p)) return 0;
  if (!num(p, end, ",\"sub\":", &sub, &p)) return 0;
  if (ep < 0 || ep > UINT32_MAX || ev < INT32_MIN || ev > INT32_MAX || sub < 0 || sub > 255) return 0;
  k->ep = (uint32_t)ep; k->ev = (int32_t)ev; k->sub = (uint8_t)sub;
  return 1;
}

int jsonl_last_key(const char *buf, size_t n, int complete, Key *k, int *ends_nl) {
  *ends_nl = n == 0 || buf[n - 1] == '\n';
  size_t end = n;
  while (end > 0) {
    size_t start = end;
    while (start > 0 && buf[start - 1] != '\n') start--;
    int partial_head = start == 0 && !complete;
    size_t len = end - start;
    if (!partial_head && len > 0 && jsonl_key(buf + start, len, k)) return 1;
    if (start == 0) break;
    end = start - 1;
  }
  return 0;
}
