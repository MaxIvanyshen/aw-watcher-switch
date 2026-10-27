#ifndef AW_PDM_LAYOUT_H
#define AW_PDM_LAYOUT_H

/* Raw PdmPlayEvent layout (little endian). The platform adapter static-asserts the libnx struct against these. */
#define PDM_EVENT_SIZE        0x38
#define PDM_OFF_PROGRAM_ID    0x00 /* two u32 words, low/high swapped (E4) */
#define PDM_OFF_APPLET_ID     0x0c
#define PDM_OFF_STORAGE_ID    0x0d
#define PDM_OFF_LOG_POLICY    0x0e
#define PDM_OFF_APPLET_EVENT  0x0f
#define PDM_OFF_POWER_VALUE   0x00 /* event_data.power_state_change.value */
#define PDM_OFF_PLAY_TYPE     0x1c
#define PDM_OFF_TS_USER       0x20
#define PDM_OFF_TS_NETWORK    0x28
#define PDM_OFF_TS_STEADY     0x30

#endif
