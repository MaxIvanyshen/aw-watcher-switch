#ifndef AWSYNC_PRIV_H
#define AWSYNC_PRIV_H
#include "sync_engine.h"

/* cfg.c */
int sync_cfg_validate(const SyncCfg *c);
int sync_cfg_paths_ok(const SyncCfg *c);               /* state_dir and journal only: gate for every write */
int sync_path_ok(const char *p);                       /* absolute, [A-Za-z0-9._-/], no empty or dot components */
int sync_path_under(const char *path, const char *dir);/* component-wise, case-insensitive (FAT) */

/* ckpt.c */
void ckpt_encode(const SyncCkpt *c, uint8_t out[128]);
int  ckpt_decode(const uint8_t in[128], SyncCkpt *c);  /* 0 or -EINVAL */

/* spool.c */
typedef struct {
    uint64_t start, end;              /* journal range [start, end) in original bytes */
    uint32_t count, body_len;
    int      more;                    /* scan stopped on the batch cap, not on EOF */
    uint32_t sk_lines, sk_bytes;      /* skipped invalid lines: all seen / inside [start,end) */
    uint32_t sk_lines_in, sk_bytes_in;
    uint32_t held;                    /* unterminated invalid tail bytes */
    uint32_t anchor_len, tail_len;    /* snapshot of the exact scanned bytes, taken before any exchange */
    uint8_t  ahead[64], atail[64];
    uint8_t  anchor_sha[32], tail_sha[32], body_sha[32];
} Batch;

uint8_t *spool_body(SyncEngine *e);
int spool_line_valid(const uint8_t *p, size_t content_len);
/* Reads the journal, builds the batch body in place from ck.offset. Returns 0 or -errno. */
int spool_scan(SyncEngine *e, Batch *b);
/* Compares journal [0,alen) and [end-tlen,end) with the hashes. 0 match, 1 mismatch (short or different), <0 read error.
 * Copies the verified bytes to vh and vt when not NULL. size is the current journal size. */
int spool_verify(SyncEngine *e, uint64_t size, uint64_t end, uint32_t alen, const uint8_t *ash, uint32_t tlen,
                 const uint8_t *tsh, uint8_t *vh, uint8_t *vt);

/* http.c */
size_t http_build(SyncEngine *e, const Batch *b, const char *id, const char *shahex, uint8_t **req);
/* 0 verified ack, 1 not an ack (transient), 2 blocked status. Sets *http. conflict receives a short key for 409. */
int http_check(const uint8_t *r, size_t n, const char *id, const char *shahex, uint32_t count, int *http, char conflict[25]);
#endif
