#ifndef AWSYNC_ENGINE_H
#define AWSYNC_ENGINE_H
#include "sync_plat.h"
#include "sync_paths.h"

#define SYNC_LINE_MAX            384
#define SYNC_BATCH_MAX_BYTES     12288
#define SYNC_BATCH_MAX_RECORDS   32
#define SYNC_MAX_BATCHES_PER_RUN 40
#define SYNC_EXCHANGE_TIMEOUT_MS 20000u
#define SYNC_RUN_TIMEOUT_MS      120000u
#define SYNC_IDLE_MS             30000u
#define SYNC_BACKOFF_MIN_MS      5000u
#define SYNC_BACKOFF_MAX_MS      600000u
#define SYNC_BLOCKED_MS          1800000u

typedef struct {
    uint8_t  ip[4];            /* must be inside 100.64.0.0/10 */
    uint16_t port;
    char     device[37];       /* lowercase UUID */
    char     token[129];       /* [A-Za-z0-9._~+-]{16,128} */
    char     journal[96];      /* default SYNC_PATH_JOURNAL */
    char     state_dir[96];    /* default SYNC_DATA_DIR */
} SyncCfg;

/* config.txt: LF lines "key=value", keys server (a.b.c.d:port, numeric only), device, token, optional journal,
 * state_dir. Unknown key, duplicate key, CR, missing required key, non-tailnet IP: -EINVAL. Total size <= 512. */
int sync_cfg_parse(const char *text, size_t n, SyncCfg *out);

typedef enum { SYNC_IDLE = 0, SYNC_MORE, SYNC_RETRY, SYNC_BLOCKED } SyncOutcome;
typedef struct { SyncOutcome outcome; uint32_t delay_ms; } SyncRun;

typedef struct SyncEngine SyncEngine;   /* defined in sync_engine.h by A below this line, ~14 KiB, platform allocates statically */
void   sync_init(SyncEngine *e, const SyncPlat *p, const SyncCfg *cfg);
/* One bounded pass: verify checkpoint and anchors, scan, send up to SYNC_MAX_BATCHES_PER_RUN batches over one
 * net_open/net_close pair, update checkpoint and status. Never loops past SYNC_RUN_TIMEOUT_MS.
 * The platform main loop is: for (;;) { r = sync_run_once(&e); sleep(r.delay_ms); }
 * IDLE: nothing pending. MORE: capped, more pending. RETRY: transient failure, backoff grows and resets on progress.
 * BLOCKED: server said no (400/401/403/409/413) or config invalid. */
SyncRun sync_run_once(SyncEngine *e);

/* ---- private part: platform treats SyncEngine as opaque, allocate it statically and pass it to the two calls above ---- */
#define SYNC_HDR_MAX 640
#define SYNC_WIN     1024
#define SYNC_RESP    1024

typedef struct {
    uint32_t seq;
    uint64_t offset;           /* first unacked journal byte */
    uint64_t acked;
    uint32_t anchor_len;       /* == min(64, offset) */
    uint32_t tail_len;         /* == min(64, offset) */
    uint8_t  anchor_sha[32];   /* sha256 of journal [0, anchor_len) */
    uint8_t  tail_sha[32];     /* sha256 of journal [offset - tail_len, offset) */
} SyncCkpt;

struct SyncEngine {
    SyncPlat plat;
    SyncCfg  cfg;
    SyncCkpt ck;
    uint8_t  loaded, next_slot, net_up;
    uint8_t  vhead[64], vtail[64];     /* verified bytes behind ck: journal [0,anchor_len) and [offset-tail_len,offset) */
    uint32_t backoff_ms, attempts, resets;
    uint32_t skipped_lines, skipped_bytes;   /* acked, since boot */
    uint32_t trail_lines, trail_bytes, held_bytes;   /* seen in the last scan, beyond the last included line */
    int      last_errno, last_http;
    char     conflict[25], stage[8], result[12];
    uint64_t win_off;
    uint32_t win_len;
    uint8_t  win_full, win_valid;
    uint8_t  resp[SYNC_RESP];
    uint8_t  win[SYNC_WIN];
    uint8_t  req[SYNC_HDR_MAX + SYNC_BATCH_MAX_BYTES];   /* header is built right-aligned in front of the body */
};
#endif
