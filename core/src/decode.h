#ifndef AW_DECODE_H
#define AW_DECODE_H

#include "watcher.h"

enum { PT_APPLET = 0, PT_ACCOUNT = 1, PT_POWER = 2, PT_OPMODE = 3, PT_INIT = 4 };
enum { AE_LAUNCH = 0, AE_EXIT = 1, AE_IN_FOCUS = 2, AE_OUT_OF_FOCUS = 3, AE_OUT_OF_FOCUS4 = 4, AE_EXIT5 = 5, AE_EXIT6 = 6 };
enum { POLICY_ALL = 0, POLICY_LOG_ONLY = 1, POLICY_NONE = 2 };
#define APPLET_ID_APPLICATION 0x01

typedef struct {
  uint8_t play_type, applet_id, storage_id, log_policy, applet_event, power_value;
  uint64_t pid, ts_user, ts_net, ts_steady;
} Event;

void decode_event(const RawEvent *r, Event *e);

#endif
