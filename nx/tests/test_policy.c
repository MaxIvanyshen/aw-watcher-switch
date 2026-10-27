/* Host tests for nx/include/policy.h. Run with nx/test.sh. No libnx, no core. */
#include <stdio.h>
#include <string.h>

#include "policy.h"

static int fails;

static void ck(int cond, const char *what)
{
    if (!cond) { printf("FAIL %s\n", what); fails++; }
}

/* F2: repeated write failures with successful reads in between must still reach the remount threshold. */
static void test_write_streak_ignores_reads(void)
{
    NxPolicy p;
    nx_policy_init(&p);

    /* The exact core call pattern: append fails, journal is dirty, next attempt tail-reads first. */
    nx_policy_write_result(&p, 5); /* EIO append */
    nx_policy_read_result(&p, 0);  /* tail read succeeded */
    ck(p.write_streak == 1, "one failed write, one streak point");
    ck(!nx_policy_should_remount(&p), "no remount after one failure");

    nx_policy_write_result(&p, 5);
    nx_policy_read_result(&p, 0);
    ck(p.write_streak == 2, "the read success did not clear the streak");
    ck(!nx_policy_should_remount(&p), "no remount after two failures");

    nx_policy_write_result(&p, 5);
    nx_policy_read_result(&p, 0);
    ck(nx_policy_should_remount(&p), "three write failures with successful reads remount");

    for (int i = 0; i < 20; i++) {
        nx_policy_write_result(&p, 5);
        nx_policy_read_result(&p, 0);
    }
    ck(p.write_streak == 23, "write streak counts every failed write");
    ck(p.read_streak == 0, "reads kept succeeding");
}

static void test_write_streak_resets_on_write_success(void)
{
    NxPolicy p;
    nx_policy_init(&p);
    nx_policy_write_result(&p, 1);
    nx_policy_write_result(&p, 1);
    ck(!nx_policy_should_remount(&p), "two failures do not remount");
    nx_policy_write_result(&p, 2);
    ck(nx_policy_should_remount(&p), "three failures remount");
    nx_policy_write_result(&p, 0);
    ck(!nx_policy_should_remount(&p), "a successful write clears the streak");
    nx_policy_write_result(&p, 1);
    ck(!nx_policy_should_remount(&p), "streak restarts from zero after success");
    nx_policy_after_remount(&p);
    ck(p.write_streak == 0, "remount clears the streak");
}

static void test_read_streak_is_separate(void)
{
    NxPolicy p;
    nx_policy_init(&p);
    for (int i = 0; i < 10; i++) nx_policy_read_result(&p, 5);
    ck(p.read_streak == 10, "read failures count");
    ck(!nx_policy_should_remount(&p), "read failures alone do not trigger a remount");
    nx_policy_read_result(&p, 0);
    ck(p.read_streak == 0, "read success clears the read streak");
}

static void test_backoff(void)
{
    uint32_t cur = 0;
    ck(nx_policy_backoff(&cur) == NX_RETRY_MIN_MS, "first wait is the minimum");
    ck(nx_policy_backoff(&cur) == 2000, "doubles");
    ck(nx_policy_backoff(&cur) == 4000, "doubles again");
    for (int i = 0; i < 20; i++) {
        uint32_t v = nx_policy_backoff(&cur);
        ck(v <= NX_RETRY_MAX_MS, "never above the maximum");
    }
    ck(nx_policy_backoff(&cur) == NX_RETRY_MAX_MS, "sticks at the maximum");
    ck(cur == NX_RETRY_MAX_MS, "state stays at the maximum");
}

static void test_diag_cap(void)
{
    ck(nx_policy_diag_fits(0, 100), "empty file fits a line");
    ck(nx_policy_diag_fits(NX_DIAG_CAP_BYTES - 100, 100), "exactly to the cap fits");
    ck(!nx_policy_diag_fits(NX_DIAG_CAP_BYTES - 99, 100), "one byte over the cap does not fit");
    ck(!nx_policy_diag_fits(NX_DIAG_CAP_BYTES, 1), "a full file fits nothing, not even the marker");
    ck(!nx_policy_diag_fits(NX_DIAG_CAP_BYTES + 1, 0), "an oversized file stays closed");
    ck(!nx_policy_diag_fits(0xffffffffu, 1), "no wraparound at the top of the range");
}

int main(void)
{
    test_write_streak_ignores_reads();
    test_write_streak_resets_on_write_success();
    test_read_streak_is_separate();
    test_backoff();
    test_diag_cap();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("policy: all checks passed\n");
    return 0;
}
