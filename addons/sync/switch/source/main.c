// aw-sync sysmodule entry. Services: fsp-srv, set:sys, bsd:u only. No applet, hid, time, nifm, psc, dns.
#include <switch.h>
#include <string.h>

#include "platform.h"
#include "sync_engine.h"

#define HEAP_SIZE 0x40000
#define CFG_MAX 512
#define SETUP_BACKOFF_MAX_MS 10000
#define MIN_DELAY_MS 100

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static u8 heap[HEAP_SIZE];
    extern void *fake_heap_start, *fake_heap_end;
    fake_heap_start = heap;
    fake_heap_end = heap + sizeof heap;
}

void __appInit(void) {} // sm/fs/version are set up with retries in main()

void __appExit(void) {}

// sm/fs/version/sdmc can be missing early in boot; every step is safe to repeat.
static bool setup(void) {
    static bool sm_ok, fs_ok, hos_ok, sdmc_ok;
    if (!sm_ok) sm_ok = R_SUCCEEDED(smInitialize());
    if (sm_ok && !fs_ok) fs_ok = R_SUCCEEDED(fsInitialize());
    if (sm_ok && !hos_ok && R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            hos_ok = true;
        }
        setsysExit();
    }
    if (fs_ok && !sdmc_ok) sdmc_ok = R_SUCCEEDED(fsdevMountSdmc());
    return sm_ok && fs_ok && hos_ok && sdmc_ok;
}

static void sleep_ms(uint32_t ms) {
    if (ms < MIN_DELAY_MS) ms = MIN_DELAY_MS;
    svcSleepThread((s64)ms * 1000000LL);
}

static int load_cfg(const SyncPlat *p, SyncCfg *out) {
    char text[CFG_MAX + 2];
    size_t got = 0;
    int rc = p->read_at(p->ctx, SYNC_PATH_CONFIG, 0, text, CFG_MAX + 1, &got);
    if (rc) return rc;
    text[got] = 0;
    return sync_cfg_parse(text, got, out);
}

int main(void) {
    for (uint32_t b = 1000; !setup(); b = b * 2 > SETUP_BACKOFF_MAX_MS ? SETUP_BACKOFF_MAX_MS : b * 2) sleep_ms(b);

    static SyncPlat plat;
    static SyncCfg cfg, next;
    static SyncEngine eng;
    bool inited = false;
    switch_plat_init(&plat);

    for (;;) {
        uint32_t delay = SYNC_BLOCKED_MS;
        // Config is re-read every run. The engine copies it in sync_init, so a changed config re-inits the engine
        // (retained backoff is lost, the checkpoint is reloaded from the SD). An invalid file waits, no engine call.
        memset(&next, 0, sizeof next);
        if (load_cfg(&plat, &next) == 0) {
            if (!inited || memcmp(&cfg, &next, sizeof cfg) != 0) {
                cfg = next;
                sync_init(&eng, &plat, &cfg);
                inited = true;
            }
            delay = sync_run_once(&eng).delay_ms;
        }
        // The bsd:u session is never held while sleeping, so tailscale-nx can stop and the MITM reopens after boot.
        plat.net_close(plat.ctx);
        sleep_ms(delay);
    }
}
