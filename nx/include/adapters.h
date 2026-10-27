#pragma once
#include <stdint.h>
#include "policy.h"
#include "watcher.h"

typedef struct {
    NxPolicy policy;
    unsigned diag_lines;
    uint32_t diag_bytes;
    int diag_bytes_known;
    int diag_dead;
} NxCtx;

extern NxCtx g_nx;
int nx_ensure_data_dir(void);
extern const PdmSrc NX_PDM;
extern const Fs NX_FS;
extern const Env NX_ENV;
