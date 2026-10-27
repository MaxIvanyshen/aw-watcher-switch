#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "adapters.h"
#include "policy.h"

#define INNER_HEAP_SIZE 0x20000

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
TimeServiceType __nx_time_service_type = TimeServiceType_User;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

static bool g_sm_ok, g_fs_ok;

/* First attempt only. A failure here is retried by runtime_ready() instead of ending the process (F1). */
void __appInit(void)
{
    g_sm_ok = R_SUCCEEDED(smInitialize());
    if (g_sm_ok) g_fs_ok = R_SUCCEEDED(fsInitialize());
}

void __appExit(void)
{
    pdmqryExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static void sleep_ms(uint32_t ms) { svcSleepThread((int64_t)ms * 1000000LL); }

/* sm (and therefore fs) can be missing early in boot. Retry, never give up: the caller backs off. */
static bool runtime_ready(void)
{
    if (!g_sm_ok) g_sm_ok = R_SUCCEEDED(smInitialize());
    if (g_sm_ok && !g_fs_ok) g_fs_ok = R_SUCCEEDED(fsInitialize());
    return g_sm_ok && g_fs_ok;
}

static bool g_hos_ok, g_time_ok, g_pdm_ok;

/* hosversion must be set before libnx picks service ABIs, so time and pdm wait for it. */
static bool init_services(void)
{
    if (!g_hos_ok) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysInitialize())) {
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
                g_hos_ok = true;
            }
            setsysExit();
        }
    }
    if (g_hos_ok && !g_time_ok) g_time_ok = R_SUCCEEDED(timeInitialize());
    if (g_hos_ok && !g_pdm_ok) g_pdm_ok = R_SUCCEEDED(pdmqryInitialize());
    return g_hos_ok && g_time_ok && g_pdm_ok;
}

int main(void)
{
    static WatcherStorage storage;
    Watcher *w = (Watcher *)&storage;
    bool mounted = false, started = false;
    uint32_t rt_wait = 0, mount_wait = 0, svc_wait = 0, start_wait = 0;

    if (watcher_size() > sizeof storage) return 1;
    nx_policy_init(&g_nx.policy);

    for (;;) {
        if (!runtime_ready()) { sleep_ms(nx_policy_backoff(&rt_wait)); continue; }
        rt_wait = 0;

        if (!mounted) {
            mounted = R_SUCCEEDED(fsdevMountSdmc());
            if (!mounted) { sleep_ms(nx_policy_backoff(&mount_wait)); continue; }
            mount_wait = 0;
            nx_policy_after_remount(&g_nx.policy);
            if (nx_ensure_data_dir() != 0) {
                fsdevUnmountDevice("sdmc");
                mounted = false;
                sleep_ms(nx_policy_backoff(&mount_wait));
                continue;
            }
        }

        if (!started) {
            if (!init_services()) { sleep_ms(nx_policy_backoff(&svc_wait)); continue; }
            svc_wait = 0;
            if (watcher_init(w, NX_PDM, NULL, NX_FS, NULL, NX_ENV, NULL) != 0) {
                sleep_ms(nx_policy_backoff(&start_wait));
                continue;
            }
            started = true;
        }

        uint32_t next = 5000;
        watcher_poll(w, &next);

        /* Only consecutive failed writes reach the threshold. Successful reads do not reset it (F2). */
        if (nx_policy_should_remount(&g_nx.policy)) {
            fsdevUnmountDevice("sdmc");
            mounted = false;
            nx_policy_after_remount(&g_nx.policy);
        }
        sleep_ms(next);
    }
}
