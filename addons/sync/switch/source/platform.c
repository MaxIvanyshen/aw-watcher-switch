// libnx implementation of SyncPlat. Hardware behaviour of sleep/wake is not verified, see PLATFORM-HANDOFF.md.
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"
#include "sync_paths.h"

#define PATH_MAX_ 160
#define NET_PREFIX "sdmc:"

static bool g_net_open;

static int err_(void) { return errno > 0 ? -errno : -EIO; }

static int map_path(const char *path, char *out) {
    size_t n = strlen(path);
    if (path[0] != '/' || n + sizeof NET_PREFIX > PATH_MAX_) return -EINVAL;
    memcpy(out, NET_PREFIX, sizeof NET_PREFIX - 1);
    memcpy(out + sizeof NET_PREFIX - 1, path, n + 1);
    return 0;
}

// Writes are confined to /config/aw-watcher-switch-sync*, so the watcher's journal dir /config/aw-watcher-switch/ is unwritable.
static int writable_path(const char *path) {
    static const char root[] = SYNC_DATA_DIR; // also matches sibling dirs like ...-sync-test, never /config/aw-watcher-switch/
    if (strncmp(path, root, sizeof root - 1) != 0 || strstr(path, "..") || strstr(path, "//")) return -EPERM;
    return 0;
}

static int p_file_size(void *ctx, const char *path, uint64_t *size) {
    (void)ctx;
    char m[PATH_MAX_ + 8];
    int rc = map_path(path, m);
    if (rc) return rc;
    struct stat st;
    if (stat(m, &st) < 0) return err_();
    if (!S_ISREG(st.st_mode) || st.st_size < 0) return -EISDIR;
    *size = (uint64_t)st.st_size;
    return 0;
}

static int p_read_at(void *ctx, const char *path, uint64_t off, void *buf, size_t cap, size_t *got) {
    (void)ctx;
    *got = 0;
    char m[PATH_MAX_ + 8];
    int rc = map_path(path, m);
    if (rc) return rc;
    int fd = open(m, O_RDONLY);
    if (fd < 0) return err_();
    rc = 0;
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) rc = err_();
    size_t n = 0;
    while (rc == 0 && n < cap) {
        ssize_t r = read(fd, (uint8_t *)buf + n, cap - n);
        if (r < 0) { rc = err_(); break; }
        if (r == 0) break;
        n += (size_t)r;
    }
    if (close(fd) < 0 && rc == 0) rc = err_();
    if (rc == 0) *got = n;
    return rc;
}

static int open_for_write(const char *m) {
    int fd = open(m, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0 || errno != ENOENT) return fd;
    // Create missing parent directories (state_dir may be a subdirectory), one level at a time.
    size_t base = sizeof NET_PREFIX;
    char d[PATH_MAX_ + 8];
    strcpy(d, m);
    for (size_t i = base; d[i]; i++) {
        if (d[i] != '/') continue;
        d[i] = 0;
        if (mkdir(d, 0755) < 0 && errno != EEXIST) return -1;
        d[i] = '/';
    }
    return open(m, O_WRONLY | O_CREAT, 0644);
}

static int p_write_at(void *ctx, const char *path, uint64_t off, const void *buf, size_t n) {
    (void)ctx;
    int rc = writable_path(path);
    if (rc) return rc;
    char m[PATH_MAX_ + 8];
    if ((rc = map_path(path, m))) return rc;
    int fd = open_for_write(m);
    if (fd < 0) return err_();
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) rc = err_();
    for (size_t w = 0; rc == 0 && w < n;) {
        ssize_t r = write(fd, (const uint8_t *)buf + w, n - w);
        if (r <= 0) { rc = r < 0 ? err_() : -EIO; break; }
        w += (size_t)r;
    }
    if (rc == 0 && fsync(fd) < 0) rc = err_();
    if (close(fd) < 0 && rc == 0) rc = err_();
    return rc;
}

static uint64_t p_now_ms(void *ctx) {
    (void)ctx;
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static int p_net_open(void *ctx) {
    (void)ctx;
    if (g_net_open) return 0;
    // Static sizing: 1 bsd session, 1 buffer set, 16 KiB TCP buffers (hardware-proven in the probe). bsd:u only.
    SocketInitConfig sc = {
        .tcp_tx_buf_size = 0x4000, .tcp_rx_buf_size = 0x4000,
        .udp_tx_buf_size = 0x2400, .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 1, .num_bsd_sessions = 1,
        .bsd_service_type = BsdServiceType_User,
    };
    if (R_FAILED(socketInitialize(&sc))) return -ENETDOWN;
    g_net_open = true;
    return 0;
}

static void p_net_close(void *ctx) {
    (void)ctx;
    if (!g_net_open) return;
    socketExit();
    g_net_open = false;
}

static int left_ms(uint64_t deadline) {
    uint64_t now = p_now_ms(NULL);
    if (now >= deadline) return 0;
    uint64_t d = deadline - now;
    return d > 60000 ? 60000 : (int)d;
}

// Waits for ev until the absolute deadline. A socket error is returned as -SO_ERROR.
static int wait_fd(int fd, short ev, uint64_t deadline) {
    int ms = left_ms(deadline);
    if (ms == 0) return -ETIMEDOUT;
    struct pollfd p = {fd, ev, 0};
    int r = poll(&p, 1, ms);
    if (r == 0) return -ETIMEDOUT;
    if (r < 0) return err_();
    if (p.revents & (POLLERR | POLLNVAL)) {
        int se = 0;
        socklen_t sl = sizeof se;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl) == 0 && se) return -se;
        return -ECONNRESET;
    }
    return 0;
}

static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }

static int p_exchange(void *ctx, const uint8_t ip[4], uint16_t port, const void *req, size_t req_len,
                      void *resp, size_t resp_cap, size_t *resp_len, uint64_t deadline) {
    (void)ctx;
    *resp_len = 0;
    if (!g_net_open) return -ENETDOWN;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return err_();
    int rc = 0;
    size_t n = 0;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { rc = err_(); goto out; }

    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    memcpy(&sa.sin_addr, ip, 4);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS && !would_block()) { rc = err_(); goto out; }
        if ((rc = wait_fd(fd, POLLOUT, deadline))) goto out;
        int se = 0;
        socklen_t sl = sizeof se;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl) < 0) { rc = err_(); goto out; }
        if (se) { rc = -se; goto out; }
    }

    for (size_t off = 0; off < req_len;) {
        if (left_ms(deadline) == 0) { rc = -ETIMEDOUT; goto out; }
        ssize_t w = send(fd, (const uint8_t *)req + off, req_len - off, 0);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && would_block()) { if ((rc = wait_fd(fd, POLLOUT, deadline))) goto out; continue; }
        rc = w < 0 ? err_() : -EIO;
        goto out;
    }

    for (;;) {
        if (left_ms(deadline) == 0) { rc = -ETIMEDOUT; goto out; }
        uint8_t extra;
        bool full = n == resp_cap;
        ssize_t r = recv(fd, full ? &extra : (uint8_t *)resp + n, full ? 1 : resp_cap - n, 0);
        if (r == 0) break;
        if (r > 0) {
            if (full) { rc = -EMSGSIZE; goto out; }
            n += (size_t)r;
            continue;
        }
        if (!would_block()) { rc = err_(); goto out; }
        if ((rc = wait_fd(fd, POLLIN, deadline))) goto out;
    }
out:
    close(fd);
    if (rc == 0) *resp_len = n;
    return rc;
}

void switch_plat_init(SyncPlat *p) {
    *p = (SyncPlat){
        .ctx = NULL, .file_size = p_file_size, .read_at = p_read_at, .write_at = p_write_at,
        .now_ms = p_now_ms, .net_open = p_net_open, .net_close = p_net_close, .exchange = p_exchange,
    };
}
