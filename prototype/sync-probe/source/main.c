// Throwaway: can an hbmenu NRO POST to a tailnet endpoint via tailscale-nx? No session data.
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

#define CONF "sdmc:/config/aw-watcher-switch-sync-probe/config.txt"
#define BODY "{\"probe\":\"aw-watcher-switch\",\"v\":1}"
#define TMO_MS 5000
#define TOKEN_MAX 128

static char ip[32], tok[TOKEN_MAX + 2];
static int port;

static int readcfg(void) {
    FILE *f = fopen(CONF, "r");
    char pb[16];
    if (!f) { printf("config: cannot open (errno %d)\n", errno); return -1; }
    char *a = fgets(ip, sizeof ip, f), *b = fgets(pb, sizeof pb, f), *c = fgets(tok, sizeof tok, f);
    fclose(f);
    if (!a || !b || !c) { puts("config: need 3 lines (or a line too long)"); return -1; }
    ip[strcspn(ip, "\r\n")] = 0; pb[strcspn(pb, "\r\n")] = 0; tok[strcspn(tok, "\r\n")] = 0;
    struct in_addr t;
    char *end;
    long p = strtol(pb, &end, 10);
    if (inet_pton(AF_INET, ip, &t) != 1) { puts("config: bad IPv4"); return -1; }
    if (*pb == 0 || *end || p < 1 || p > 65535) { puts("config: bad port"); return -1; }
    size_t n = strlen(tok);
    if (n < 1 || n > TOKEN_MAX) { puts("config: token length must be 1..128"); return -1; }
    for (size_t i = 0; i < n; i++)
        if (tok[i] < 0x21 || tok[i] > 0x7e) { puts("config: token has non-printable/space char"); return -1; }
    port = (int)p;
    printf("config ok: %s:%d token=%zu chars\n", ip, port, n);
    return 0;
}

static int wait_fd(int fd, short ev) {
    struct pollfd p = {fd, ev, 0};
    int r = poll(&p, 1, TMO_MS);
    if (r == 0) { errno = ETIMEDOUT; return -1; }
    return r < 0 ? -1 : 0;
}

static void probe(void) {
    char req[512], resp[512];
    int fd = -1, len = snprintf(req, sizeof req,
        "POST /probe HTTP/1.1\r\nHost: %s:%d\r\nAuthorization: Bearer %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" BODY,
        ip, port, tok, (int)strlen(BODY));
    if (len <= 0 || len >= (int)sizeof req) { puts("request too large"); return; }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("socket failed errno=%d\n", errno); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, ip, &sa.sin_addr);
    if (connect(fd, (void *)&sa, sizeof sa) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) { printf("connect failed errno=%d\n", errno); goto out; }
        if (wait_fd(fd, POLLOUT) < 0) { printf("connect wait failed errno=%d\n", errno); goto out; }
        int se = 0; socklen_t sl = sizeof se;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &se, &sl);
        if (se) { printf("connect failed so_error=%d\n", se); goto out; }
    }
    puts("connected");
    for (int off = 0; off < len;) {
        ssize_t w = send(fd, req + off, len - off, 0);
        if (w > 0) { off += w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(fd, POLLOUT) == 0) continue;
        printf("send failed errno=%d\n", errno); goto out;
    }
    int n = 0;
    while (n < (int)sizeof resp - 1) {
        ssize_t r = recv(fd, resp + n, sizeof resp - 1 - n, 0);
        if (r > 0) { n += r; if (memchr(resp, '\n', n)) break; continue; }
        if (r == 0) break;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && wait_fd(fd, POLLIN) == 0) continue;
        printf("recv failed errno=%d\n", errno); goto out;
    }
    resp[n] = 0;
    char *nl = strpbrk(resp, "\r\n");
    if (nl) *nl = 0;
    printf("response: \"%.100s\" -> %s\n", resp, strncmp(resp, "HTTP/1.1 200", 12) == 0 || strncmp(resp, "HTTP/1.0 200", 12) == 0 ? "PASS" : "FAIL");
out:
    close(fd);
}

int main(void) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    SocketInitConfig sc = *socketGetDefaultInitConfig();
    sc.bsd_service_type = BsdServiceType_User; // tailscale-nx hooks bsd:u only
    Result rc = socketInitialize(&sc);
    printf("aw sync probe (A=send, PLUS=exit)\nsocketInitialize rc=0x%x\n", rc);
    int ok = R_SUCCEEDED(rc) && readcfg() == 0;
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & HidNpadButton_Plus) break;
        if ((d & HidNpadButton_A) && ok) { puts("sending..."); consoleUpdate(NULL); probe(); puts("done"); }
        consoleUpdate(NULL);
    }
    socketExit();
    consoleExit(NULL);
    return 0;
}
