#ifndef AW_NX_POLICY_H
#define AW_NX_POLICY_H

/* Platform error and retry policy. No libnx dependency, so it is host-testable (see nx/test.sh). */

#include <stddef.h>
#include <stdint.h>

#define NX_WRITE_FAILS_BEFORE_REMOUNT 3u
#define NX_RETRY_MIN_MS 1000u
#define NX_RETRY_MAX_MS 30000u
#define NX_DIAG_CAP_BYTES 2097152u /* 2 MiB per diag.jsonl file, across boots */

typedef struct {
    unsigned write_streak; /* consecutive failed persistence writes (append_line, write_file) */
    unsigned read_streak;  /* consecutive failed reads, informational only */
} NxPolicy;

void nx_policy_init(NxPolicy *p);

/* Persistence result. 0 clears the write streak. A read success must never clear it (F2). */
void nx_policy_write_result(NxPolicy *p, int code);
void nx_policy_read_result(NxPolicy *p, int code);

int nx_policy_should_remount(const NxPolicy *p);
void nx_policy_after_remount(NxPolicy *p);

/* Sleep before the next attempt, doubling from the minimum up to the maximum. */
uint32_t nx_policy_backoff(uint32_t *cur);

/* 1 when appending line_len bytes keeps diag.jsonl within NX_DIAG_CAP_BYTES. */
int nx_policy_diag_fits(uint32_t file_bytes, size_t line_len);

#endif
