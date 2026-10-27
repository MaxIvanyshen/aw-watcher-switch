#include "decode.h"

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

void decode_event(const RawEvent *r, Event *e) {
  const uint8_t *b = r->b;
  /* E4: libnx documents the two u32 words of each id as low/high swapped. */
  e->pid = (uint64_t)le32(b + PDM_OFF_PROGRAM_ID) << 32 | le32(b + PDM_OFF_PROGRAM_ID + 4);
  e->applet_id = b[PDM_OFF_APPLET_ID];
  e->storage_id = b[PDM_OFF_STORAGE_ID];
  e->log_policy = b[PDM_OFF_LOG_POLICY];
  e->applet_event = b[PDM_OFF_APPLET_EVENT];
  e->power_value = b[PDM_OFF_POWER_VALUE];
  e->play_type = b[PDM_OFF_PLAY_TYPE];
  e->ts_user = le64(b + PDM_OFF_TS_USER);
  e->ts_net = le64(b + PDM_OFF_TS_NETWORK);
  e->ts_steady = le64(b + PDM_OFF_TS_STEADY);
}
