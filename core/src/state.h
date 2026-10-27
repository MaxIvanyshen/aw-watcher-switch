#ifndef AW_STATE_H
#define AW_STATE_H

#include "jsonl.h"
#include "watcher.h"

#define STATE_MAGIC 0x31535741u /* "AWS1" little endian */
#define STATE_VERSION 1
#define STATE_MAX_BYTES 128

typedef struct {
  uint32_t ep;
  int32_t cursor;
  uint32_t anchor;       /* crc32 of the raw event at cursor-1 */
  uint8_t anchor_ok;
  Tracker tr;
  uint8_t src_ok;
  uint8_t src[16];       /* steady clock source id */
  uint8_t pend_why;      /* GapWhy of a gap record not yet in the journal, or 0 */
  int32_t pend_from, pend_to;
} PState;

typedef enum { ST_OK, ST_NONE, ST_CORRUPT, ST_IOERR } StateLoad;

size_t state_encode(const PState *s, uint32_t seq, uint8_t *buf, size_t cap);
int state_decode(const uint8_t *buf, size_t n, PState *s, uint32_t *seq); /* 1 if valid */

/* Loads the valid slot with the higher seq. *next_slot is the slot to write next (0 = a, 1 = b). */
StateLoad state_load(const Fs *fs, void *fu, PState *s, uint32_t *seq, int *next_slot);
int state_save(const Fs *fs, void *fu, const PState *s, uint32_t seq, int slot);

#endif
