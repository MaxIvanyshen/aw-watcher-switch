#include "fake_pdm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "decode.h"

void fp_init(FakePdm *p) {
  p->start = p->end = 0;
  p->end_inclusive = p->range_err = p->range_garbage = 0;
  p->query_err = p->query_err_n = p->query_err_from = p->query_err_limit = 0;
  p->past_end_err = 0;
  p->range_calls = p->query_calls = 0;
}

void fp_push(FakePdm *p, const RawEvent *e) {
  if (p->end >= FP_MAX) abort();
  p->ev[p->end++] = *e;
}

void fp_expire(FakePdm *p, int32_t n) { p->start = p->start + n > p->end ? p->end : p->start + n; }

void fp_reset(FakePdm *p) { p->start = p->end = 0; }

static int pdm_range(void *u, int32_t *total, int32_t *start, int32_t *end) {
  FakePdm *p = u;
  p->range_calls++;
  if (p->range_err) return p->range_err;
  *total = p->end - p->start;
  *start = p->start;
  *end = p->end_inclusive && p->end > 0 ? p->end - 1 : p->end;
  switch (p->range_garbage) {
  case 1: *start = 50; *end = 10; break;
  case 2: *start = -5; break;
  case 3: *end = INT32_MAX; break;
  case 4: *total = -1; break;
  default: break;
  }
  return 0;
}

static int pdm_query(void *u, int32_t idx, RawEvent *out, int32_t max, int32_t *n) {
  FakePdm *p = u;
  p->query_calls++;
  if (p->query_err && p->query_err_n != 0 && idx >= p->query_err_from &&
      (p->query_err_limit == 0 || idx < p->query_err_limit)) {
    if (p->query_err_n > 0) p->query_err_n--;
    return p->query_err;
  }
  *n = 0;
  if (idx >= p->end && p->past_end_err) return EINVAL;
  for (int32_t i = 0; i < max && idx + i < p->end; i++) {
    if (idx + i < p->start) { if (i == 0) return EINVAL; break; }
    out[i] = p->ev[idx + i];
    (*n)++;
  }
  return 0;
}

PdmSrc fp_iface(void) {
  PdmSrc s = {pdm_range, pdm_query};
  return s;
}

static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

RawEvent ev_raw(uint8_t play_type, uint64_t pid, uint8_t applet_id, uint8_t policy, uint8_t applet_ev, uint8_t power,
                uint64_t user, uint64_t net, uint64_t steady) {
  RawEvent r;
  memset(&r, 0, sizeof r);
  if (play_type == PT_POWER) r.b[PDM_OFF_POWER_VALUE] = power;
  else if (play_type != PT_INIT) {
    w32(r.b + PDM_OFF_PROGRAM_ID, (uint32_t)(pid >> 32));
    w32(r.b + PDM_OFF_PROGRAM_ID + 4, (uint32_t)pid);
  }
  r.b[PDM_OFF_APPLET_ID] = applet_id;
  r.b[PDM_OFF_STORAGE_ID] = 5;
  r.b[PDM_OFF_LOG_POLICY] = policy;
  r.b[PDM_OFF_APPLET_EVENT] = applet_ev;
  r.b[PDM_OFF_PLAY_TYPE] = play_type;
  w64(r.b + PDM_OFF_TS_USER, user);
  w64(r.b + PDM_OFF_TS_NETWORK, net);
  w64(r.b + PDM_OFF_TS_STEADY, steady);
  return r;
}

RawEvent ev_app_pol(uint64_t pid, uint8_t applet_ev, uint8_t policy, uint64_t steady) {
  return ev_raw(PT_APPLET, pid, APPLET_ID_APPLICATION, policy, applet_ev, 0, 1700000000 + steady, 1700000000 + steady, steady);
}

RawEvent ev_app(uint64_t pid, uint8_t applet_ev, uint64_t steady) { return ev_app_pol(pid, applet_ev, POLICY_ALL, steady); }

RawEvent ev_power(uint8_t value, uint64_t steady) { return ev_raw(PT_POWER, 0, 0, 0, 0, value, 1700000000 + steady, 0, steady); }

RawEvent ev_init(uint64_t steady) { return ev_raw(PT_INIT, 0, 0, 0, 0, 0, 1700000000 + steady, 0, steady); }

RawEvent ev_account(uint64_t steady) {
  RawEvent r = ev_raw(PT_ACCOUNT, 0x1111222233334444ull, 0xaa, 0, 0, 0, 1700000000 + steady, 0, steady);
  memset(r.b + 8, 0xab, 16);
  r.b[PDM_OFF_PLAY_TYPE] = PT_ACCOUNT;
  return r;
}
