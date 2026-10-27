#ifndef AW_JSONL_H
#define AW_JSONL_H

#include "tracker.h"

#define JSONL_MAX 384 /* longest journal line, including '\n' */

enum { SUB_GAP = 0, SUB_SESSION = 1 };

typedef struct { uint32_t ep; int32_t ev; uint8_t sub; } Key;

typedef enum {
  GAP_NONE = 0, GAP_FIRST_RUN, GAP_STATE_LOST, GAP_STATE_CORRUPT, GAP_LOG_DROPPED, GAP_LOG_RESET, GAP_STEADY_RESET, GAP_COUNT
} GapWhy;

int key_cmp(const Key *a, const Key *b);
const char *gap_why_name(unsigned why);

/* Encode one line with a trailing CRC and '\n'. Return the length, or 0 if it does not fit in cap. */
size_t jsonl_session(char *buf, size_t cap, const Key *k, const SessionRec *r);
size_t jsonl_gap(char *buf, size_t cap, const Key *k, GapWhy why, int32_t from, int32_t to);

/* A line (without '\n') is valid when it ends in ,"c":"<8 hex>"} and the CRC matches the bytes before it. */
int jsonl_valid(const char *line, size_t n);
int jsonl_key(const char *line, size_t n, Key *k);

/* Find the last valid line in a tail read. complete = the buffer starts at the start of the file.
 * *ends_nl is set when the data is empty or ends in '\n' (otherwise the next append must fence). */
int jsonl_last_key(const char *buf, size_t n, int complete, Key *k, int *ends_nl);

#endif
