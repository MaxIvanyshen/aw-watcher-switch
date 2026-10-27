#ifndef AWSYNC_POSIX_PLAT_H
#define AWSYNC_POSIX_PLAT_H
#include "sync_plat.h"
typedef struct { char root[240]; int override; uint8_t oip[4]; uint16_t oport; } PosixCtx;   /* override: connect there instead of cfg ip:port (loopback E2E) */
/* Maps every SD-root relative path under root. Real files, real TCP sockets. */
void posix_plat_init(SyncPlat *p, PosixCtx *c, const char *root);
#endif
