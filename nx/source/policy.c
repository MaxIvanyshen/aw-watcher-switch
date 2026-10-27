#include "policy.h"

void nx_policy_init(NxPolicy *p)
{
    p->write_streak = 0;
    p->read_streak = 0;
}

void nx_policy_write_result(NxPolicy *p, int code)
{
    if (code == 0) p->write_streak = 0;
    else p->write_streak++;
}

void nx_policy_read_result(NxPolicy *p, int code)
{
    if (code == 0) p->read_streak = 0;
    else p->read_streak++;
}

int nx_policy_should_remount(const NxPolicy *p)
{
    return p->write_streak >= NX_WRITE_FAILS_BEFORE_REMOUNT;
}

void nx_policy_after_remount(NxPolicy *p) { p->write_streak = 0; }

uint32_t nx_policy_backoff(uint32_t *cur)
{
    uint32_t v = *cur ? *cur : NX_RETRY_MIN_MS;
    *cur = v >= NX_RETRY_MAX_MS / 2 ? NX_RETRY_MAX_MS : v * 2;
    return v;
}

int nx_policy_diag_fits(uint32_t file_bytes, size_t line_len)
{
    if (file_bytes >= NX_DIAG_CAP_BYTES) return 0;
    return line_len <= (size_t)(NX_DIAG_CAP_BYTES - file_bytes);
}
