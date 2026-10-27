#ifndef AWSYNC_PLAT_H
#define AWSYNC_PLAT_H
#include <stddef.h>
#include <stdint.h>

/* Every function returns 0 or -errno. Paths are SD-root relative, the platform maps them (for example "sdmc:" prefix).
 * Every call is bounded. No function may block without a deadline. */
typedef struct SyncPlat {
    void *ctx;
    int (*file_size)(void *ctx, const char *path, uint64_t *size);              /* -ENOENT if absent */
    /* open, read at most cap bytes starting at off, close. *got < cap only at EOF. */
    int (*read_at)(void *ctx, const char *path, uint64_t off, void *buf, size_t cap, size_t *got);
    /* create if absent, write n bytes at off, flush and fsync, close. Never truncates. */
    int (*write_at)(void *ctx, const char *path, uint64_t off, const void *buf, size_t n);
    uint64_t (*now_ms)(void *ctx);                                              /* monotonic */
    int  (*net_open)(void *ctx);                                                /* socketInitialize on bsd:u, idempotent */
    void (*net_close)(void *ctx);                                               /* socketExit, idempotent */
    /* New TCP connection to ip:port, send all req bytes, read until the peer closes or resp_cap is full
     * (full without close returns -EMSGSIZE). Absolute deadline in now_ms units covers connect, send and recv
     * (-ETIMEDOUT). Platform closes the socket on every return path. */
    int (*exchange)(void *ctx, const uint8_t ip[4], uint16_t port, const void *req, size_t req_len,
                    void *resp, size_t resp_cap, size_t *resp_len, uint64_t deadline_ms);
} SyncPlat;
#endif
