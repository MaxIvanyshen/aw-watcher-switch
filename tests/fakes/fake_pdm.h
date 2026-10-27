#ifndef FAKE_PDM_H
#define FAKE_PDM_H

#include "watcher.h"

#define FP_MAX 8192

typedef struct {
  RawEvent ev[FP_MAX];
  int32_t start, end;       /* entries [start, end) exist */
  int end_inclusive;        /* range() reports end-1 */
  int range_err;
  int range_garbage;        /* 1: start>end, 2: negative start, 3: huge end, 4: negative total */
  int query_err, query_err_n, query_err_from; /* query fails (n times, -1 = always) when idx >= query_err_from */
  int query_err_limit;      /* optional exclusive upper bound on failing indexes, 0 = none */
  int past_end_err;         /* querying idx >= end returns an error instead of 0 events */
  uint32_t range_calls, query_calls;
} FakePdm;

void fp_init(FakePdm *p);
PdmSrc fp_iface(void);
void fp_push(FakePdm *p, const RawEvent *e);
void fp_expire(FakePdm *p, int32_t n);
void fp_reset(FakePdm *p);          /* log restarts empty at index 0 */

/* Raw event builders. Times are steady seconds, user/network default to steady + a fixed offset. */
RawEvent ev_raw(uint8_t play_type, uint64_t pid, uint8_t applet_id, uint8_t policy, uint8_t applet_ev, uint8_t power,
                uint64_t user, uint64_t net, uint64_t steady);
RawEvent ev_app(uint64_t pid, uint8_t applet_ev, uint64_t steady);
RawEvent ev_app_pol(uint64_t pid, uint8_t applet_ev, uint8_t policy, uint64_t steady);
RawEvent ev_power(uint8_t value, uint64_t steady);
RawEvent ev_init(uint64_t steady);
RawEvent ev_account(uint64_t steady);

#endif
