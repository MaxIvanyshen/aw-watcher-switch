#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "adapters.h"
#include "paths.h"
#include "policy.h"

_Static_assert(sizeof(PdmPlayEvent) == PDM_EVENT_SIZE, "PdmPlayEvent size");
_Static_assert(sizeof(RawEvent) >= PDM_EVENT_SIZE, "RawEvent size");
_Static_assert(offsetof(PdmPlayEvent, event_data.applet.applet_id) == PDM_OFF_APPLET_ID, "applet_id");
_Static_assert(offsetof(PdmPlayEvent, event_data.applet.storage_id) == PDM_OFF_STORAGE_ID, "storage_id");
_Static_assert(offsetof(PdmPlayEvent, event_data.applet.log_policy) == PDM_OFF_LOG_POLICY, "log_policy");
_Static_assert(offsetof(PdmPlayEvent, event_data.applet.event_type) == PDM_OFF_APPLET_EVENT, "event_type");
_Static_assert(offsetof(PdmPlayEvent, event_data.power_state_change.value) == PDM_OFF_POWER_VALUE, "power value");
_Static_assert(offsetof(PdmPlayEvent, play_event_type) == PDM_OFF_PLAY_TYPE, "play_event_type");
_Static_assert(offsetof(PdmPlayEvent, timestamp_user) == PDM_OFF_TS_USER, "timestamp_user");
_Static_assert(offsetof(PdmPlayEvent, timestamp_network) == PDM_OFF_TS_NETWORK, "timestamp_network");
_Static_assert(offsetof(PdmPlayEvent, timestamp_steady) == PDM_OFF_TS_STEADY, "timestamp_steady");

#define SD_PREFIX "sdmc:"
#define PATH_MAX_NX 160
#define DIAG_FSYNC_EVERY 8

NxCtx g_nx;

static int rc_to_int(Result rc) { return rc ? (int)rc : 0; }
static int err(void) { return errno ? errno : EIO; }

static int full_path(const char *p, char *out, size_t cap)
{
    size_t pre = sizeof(SD_PREFIX) - 1, n = strlen(p);
    if (p[0] != '/' || pre + n + 1 > cap) return ENAMETOOLONG;
    memcpy(out, SD_PREFIX, pre);
    memcpy(out + pre, p, n + 1);
    return 0;
}

static void mkdirs(char *full)
{
    for (char *c = full + sizeof(SD_PREFIX); *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        mkdir(full, 0777);
        *c = '/';
    }
}

static FILE *open_mk(char *full, const char *mode)
{
    FILE *f = fopen(full, mode);
    if (!f && errno == ENOENT && mode[0] != 'r') {
        mkdirs(full);
        f = fopen(full, mode);
    }
    return f;
}

int nx_ensure_data_dir(void)
{
    char full[PATH_MAX_NX];
    int rc = full_path(AW_DATA_DIR "/x", full, sizeof full);
    if (rc) return rc;
    mkdirs(full);
    struct stat st;
    return stat("sdmc:" AW_DATA_DIR, &st) == 0 ? 0 : err();
}

/* Persistence results drive the remount streak; reads are counted separately (F2). */
static int note_write(int code)
{
    nx_policy_write_result(&g_nx.policy, code);
    return code;
}

static int note_read(int code)
{
    nx_policy_read_result(&g_nx.policy, code);
    return code;
}

/* n == 0 is a durability barrier, not a no-op: it opens, flushes, fsyncs and closes the existing file,
 * so the core can confirm that bytes already visible in the journal are on disk after a failed flush.
 * The open+fflush+fsync+close chain below runs for n == 0 as well, and every step is checked. */
static int write_sync(const char *path, const char *mode, const void *buf, size_t n, int do_sync)
{
    char full[PATH_MAX_NX];
    int rc = full_path(path, full, sizeof full);
    if (rc) return note_write(rc);
    FILE *f = open_mk(full, mode);
    if (!f) return note_write(err());
    if (n && fwrite(buf, 1, n, f) != n) rc = err();
    if (!rc && fflush(f) != 0) rc = err();
    if (!rc && do_sync && fsync(fileno(f)) != 0) rc = err();
    if (fclose(f) != 0 && !rc) rc = err();
    return note_write(rc);
}

static int fs_append_line(void *u, const char *path, const char *buf, size_t n)
{
    (void)u;
    return write_sync(path, "ab", buf, n, 1); /* n == 0: fsync barrier */
}

static int fs_write_file(void *u, const char *path, const void *buf, size_t n)
{
    (void)u;
    return write_sync(path, "wb", buf, n, 1);
}

static int read_from(const char *path, void *buf, size_t cap, size_t *n, int tail)
{
    char full[PATH_MAX_NX];
    int rc = full_path(path, full, sizeof full);
    *n = 0;
    if (rc) return note_read(rc);
    FILE *f = fopen(full, "rb");
    if (!f) return note_read(err());
    if (tail) {
        if (fseek(f, 0, SEEK_END) != 0) rc = err();
        else {
            long sz = ftell(f);
            if (sz < 0) rc = err();
            else if ((size_t)sz > cap && fseek(f, sz - (long)cap, SEEK_SET) != 0) rc = err();
            else if ((size_t)sz <= cap && fseek(f, 0, SEEK_SET) != 0) rc = err();
        }
    }
    if (!rc) {
        size_t got = fread(buf, 1, cap, f);
        if (ferror(f)) rc = err();
        else *n = got;
    }
    fclose(f);
    return note_read(rc);
}

static int fs_read_tail(void *u, const char *path, char *buf, size_t cap, size_t *n)
{
    (void)u;
    return read_from(path, buf, cap, n, 1);
}

static int fs_read_file(void *u, const char *path, void *buf, size_t cap, size_t *n)
{
    (void)u;
    return read_from(path, buf, cap, n, 0);
}

static int pdm_range(void *u, int32_t *total, int32_t *start, int32_t *end)
{
    (void)u;
    return rc_to_int(pdmqryGetAvailablePlayEventRange(total, start, end));
}

static int pdm_query(void *u, int32_t idx, RawEvent *out, int32_t max, int32_t *n)
{
    (void)u;
    *n = 0;
    return rc_to_int(pdmqryQueryPlayEvent(idx, (PdmPlayEvent *)out, max, n));
}

static int env_steady(void *u, int64_t *secs, uint8_t source_id[16])
{
    (void)u;
    TimeSteadyClockTimePoint tp;
    Result rc = timeGetStandardSteadyClockTimePoint(&tp);
    if (R_FAILED(rc)) return rc_to_int(rc);
    *secs = tp.time_point;
    memcpy(source_id, tp.source_id.uuid, 16);
    return 0;
}

static int env_diag_enabled(void *u)
{
    (void)u;
    char full[PATH_MAX_NX];
    struct stat st;
    if (g_nx.diag_dead || full_path(AW_PATH_DIAG_FLAG, full, sizeof full)) return 0;
    return stat(full, &st) == 0;
}

static const char DIAG_TRUNC_LINE[] = "{\"d\":\"trunc\",\"cap\":2097152}\n";

/* Core caps its per-process output; the file total is enforced here because Fs has no stat (see FORMAT.md). */
static uint32_t diag_file_bytes(void)
{
    char full[PATH_MAX_NX];
    struct stat st;
    if (g_nx.diag_bytes_known || full_path(AW_PATH_DIAG_LOG, full, sizeof full)) return 0;
    g_nx.diag_bytes_known = 1;
    g_nx.diag_bytes = stat(full, &st) == 0 && st.st_size > 0 ? (uint32_t)st.st_size : 0;
    return g_nx.diag_bytes;
}

static int diag_append(const char *line, size_t n)
{
    char full[PATH_MAX_NX];
    int rc = full_path(AW_PATH_DIAG_LOG, full, sizeof full);
    if (!rc) {
        FILE *f = open_mk(full, "ab");
        if (!f) rc = err();
        else {
            if (fwrite(line, 1, n, f) != n) rc = err();
            if (!rc && fflush(f) != 0) rc = err();
            if (!rc && ++g_nx.diag_lines % DIAG_FSYNC_EVERY == 0 && fsync(fileno(f)) != 0) rc = err();
            if (fclose(f) != 0 && !rc) rc = err();
        }
    }
    if (rc) { g_nx.diag_dead = 1; return rc; }
    g_nx.diag_bytes += (uint32_t)n;
    return 0;
}

static int env_diag_write(void *u, const char *line, size_t n)
{
    (void)u;
    if (g_nx.diag_dead) return EIO;
    if (nx_policy_diag_fits(diag_file_bytes(), n)) return diag_append(line, n);

    /* File total reached: one final marker if it still fits, then diagnostics stop until restart.
     * The flag is set regardless of the marker result, so a repeated call cannot append it again. */
    if (nx_policy_diag_fits(g_nx.diag_bytes, sizeof DIAG_TRUNC_LINE - 1))
        (void)diag_append(DIAG_TRUNC_LINE, sizeof DIAG_TRUNC_LINE - 1);
    g_nx.diag_dead = 1;
    return ENOSPC;
}

const PdmSrc NX_PDM = { pdm_range, pdm_query };
const Fs NX_FS = { fs_append_line, fs_read_tail, fs_write_file, fs_read_file };
const Env NX_ENV = { env_steady, env_diag_enabled, env_diag_write };
