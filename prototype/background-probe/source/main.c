// Throwaway finite sysmodule probe: one POST to a tailnet endpoint via tailscale-nx mitm_titles. No session data.
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DIR_ "sdmc:/config/aw-watcher-switch-sync-probe/"
#define CONF DIR_ "config.txt"
#define STATUS DIR_ "background-status.json"
#define BODY "{\"probe\":\"aw-watcher-switch\",\"v\":1}"
#define TMO_MS 5000
#define TOKEN_MAX 128
#define BACKOFF_MAX_S 10
#define TOTAL_S 180
#define HEAP_SIZE 0x40000

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

static char ip[32], tok[TOKEN_MAX + 2];
static int port;
static bool sm_ok, fs_ok, hos_ok, sdmc_ok;

static const char *g_stage = "start", *g_result = "running";
static int g_errno, g_http, g_attempts;

static void status(void) {
    if (!sdmc_ok) return;
    char b[160];
    int n = snprintf(b, sizeof b, "{\"stage\":\"%s\",\"result\":\"%s\",\"errno\":%d,\"http\":%d,\"attempts\":%d}\n",
                     g_stage, g_result, g_errno, g_http, g_attempts);
    FILE *f = fopen(STATUS, "w");
    if (!f) return;
    size_t w = fwrite(b, 1, n, f);
    int c = fclose(f);
    (void)w; (void)c; // best effort: a failed status write never affects the probe
}

static int fail(const char *stage, int err) {
    g_stage = stage; g_result = "fail"; g_errno = err;
    return -1;
}

static int readcfg(void) {
    FILE *f = fopen(CONF, "r");
    char pb[16];
    if (!f) return fail("config_open", errno);
    char *a = fgets(ip, sizeof ip, f), *b = fgets(pb, sizeof pb, f), *c = fgets(tok, sizeof tok, f);
    fclose(f);
    if (!a || !b || !c) return fail("config_format", 0);
    ip[strcspn(ip, "\r\n")] = 0; pb[strcspn(pb, "\r\n")] = 0; tok[strcspn(tok, "\r\n")] = 0;
    struct in_addr t;
    char *end;
    long p = strtol(pb, &end, 10);
    if (inet_pton(AF_INET, ip, &t) != 1) return fail("config_ip", 0);
    if (*pb == 0 || *end || p < 1 || p > 65535) return fail("config_port", 0);
    size_t n = strlen(tok);
    if (n < 1 || n > TOKEN_MAX) return fail("config_token", 0);
    for (size_t i = 0; i < n; i++)
        if (tok[i] < 0x21 || tok[i] > 0x7e) return fail("config_token", 0);
    port = (int)p;
    return 0;
}

static int wait_fd(int fd, short ev) {
    struct pollfd p = {fd, ev, 0};
    int r = poll(&p, 1, TMO_MS);
    if (r == 0) { errno = ETIMEDOUT; return -1; }
    return r < 0 ? -1 : 0;
}

static int probe(void) {
    char req[512], resp[512];
    int len = snprintf(req, sizeof req,
        "POST /probe HTTP/1.1\r\nHost: %s:%d\r\nAuthorization: Bearer %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" BODY,
        ip, port, tok, (int)strlen(BODY));
    if (len <= 0 || len >= (int)sizeof req) return fail("request", 0);

    int rv = -1;
    g_stage = "socket";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return fail("socket", errno);
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { fail("nonblock", errno); goto out; }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, ip, &sa.sin_addr);
    g_stage = "connect";
    if (connect(fd, (void *)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) { fail("connect", errno); goto out; }
        if (wait_fd(fd, POLLOUT) < 0) { fail("connect_wait", errno); goto out; }
        int se = 0; socklen_t sl = sizeof se;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl) < 0) { fail("so_error", errno); goto out; }
        if (se) { fail("connect", se); goto out; }
    }
    g_stage = "send";
    for (int off = 0; off < len;) {
        ssize_t w = send(fd, req + off, len - off, 0);
        if (w > 0) { off += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(fd, POLLOUT) == 0) continue;
        fail("send", errno); goto out;
    }
    g_stage = "recv";
    int n = 0;
    while (n < (int)sizeof resp - 1) {
        ssize_t r = recv(fd, resp + n, sizeof resp - 1 - n, 0);
        if (r > 0) { n += r; if (memchr(resp, '\n', n)) break; continue; }
        if (r == 0) break;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(fd, POLLIN) == 0) continue;
        fail("recv", errno); goto out;
    }
    resp[n] = 0;
    int code = 0;
    if (n >= 12 && strncmp(resp, "HTTP/1.", 7) == 0 && resp[8] == ' '
        && resp[9] >= '0' && resp[9] <= '9'
        && resp[10] >= '0' && resp[10] <= '9'
        && resp[11] >= '0' && resp[11] <= '9')
        code = (resp[9] - '0') * 100 + (resp[10] - '0') * 10 + resp[11] - '0';
    g_http = code;
    if (code == 200) { g_stage = "done"; g_result = "pass"; g_errno = 0; rv = 0; }
    else fail("http", 0);
out:
    close(fd);
    return rv;
}

// sm/fs/version/sdmc can be missing early in boot; every step is safe to repeat.
static bool setup(void) {
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

int main(void) {
    u64 t0 = armGetSystemTick(), hz = armGetSystemTickFreq();
    // Fixed small buffers, one buffer set: transfer memory is 0x1D000 bytes. bsd:u only (tailscale-nx hooks bsd:u).
    SocketInitConfig sc = {
        .tcp_tx_buf_size = 0x4000, .tcp_rx_buf_size = 0x4000,
        .udp_tx_buf_size = 0x2400, .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 1, .num_bsd_sessions = 1,
        .bsd_service_type = BsdServiceType_User,
    };
    u64 backoff = 1;
    for (;;) {
        g_attempts++;
        g_stage = "setup"; g_result = "running"; g_errno = 0; g_http = 0;
        int ok = -1;
        if (setup()) {
            status();
            if (readcfg() == 0) {
                g_stage = "socket_init";
                Result rc = socketInitialize(&sc);
                if (R_FAILED(rc)) fail("socket_init", (int)rc); // errno field holds the libnx Result here
                else { ok = probe(); socketExit(); }
            }
        }
        status();
        if (ok == 0) break;
        if ((armGetSystemTick() - t0) / hz + backoff >= TOTAL_S) { g_result = "gave_up"; status(); break; }
        svcSleepThread((s64)backoff * 1000000000LL);
        backoff = backoff * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff * 2;
    }
    if (sdmc_ok) fsdevUnmountAll();
    if (fs_ok) fsExit();
    if (sm_ok) smExit();
    return 0;
}
