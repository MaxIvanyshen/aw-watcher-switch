#define _POSIX_C_SOURCE 200809L
#include "posix_plat.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static int mp(const PosixCtx *c, const char *path, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s%s", c->root, path);
  return n > 0 && (size_t)n < cap ? 0 : -ENAMETOOLONG;
}

static int p_size(void *ctx, const char *path, uint64_t *size) {
  char f[512]; struct stat st;
  int rc = mp(ctx, path, f, sizeof f);
  if (rc) return rc;
  if (stat(f, &st) != 0) return -errno;
  *size = (uint64_t)st.st_size;
  return 0;
}

static int p_read(void *ctx, const char *path, uint64_t off, void *buf, size_t cap, size_t *got) {
  char f[512]; size_t n = 0; int fd, rc = mp(ctx, path, f, sizeof f);
  if (rc) return rc;
  fd = open(f, O_RDONLY);   /* the journal is only ever opened read-only */
  if (fd < 0) return -errno;
  while (n < cap) {
    ssize_t r = pread(fd, (char *)buf + n, cap - n, (off_t)(off + n));
    if (r < 0) { if (errno == EINTR) continue; rc = -errno; close(fd); return rc; }
    if (r == 0) break;
    n += (size_t)r;
  }
  close(fd);
  *got = n;
  return 0;
}

static int p_write(void *ctx, const char *path, uint64_t off, const void *buf, size_t n) {
  char f[512]; size_t w = 0; int fd, rc = mp(ctx, path, f, sizeof f);
  if (rc) return rc;
  fd = open(f, O_WRONLY | O_CREAT, 0600);
  if (fd < 0) return -errno;
  while (w < n) {
    ssize_t r = pwrite(fd, (const char *)buf + w, n - w, (off_t)(off + w));
    if (r < 0) { if (errno == EINTR) continue; rc = -errno; close(fd); return rc; }
    w += (size_t)r;
  }
  if (fsync(fd) != 0) { rc = -errno; close(fd); return rc; }
  return close(fd) == 0 ? 0 : -errno;
}

static uint64_t p_now(void *ctx) {
  struct timespec ts;
  (void)ctx;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int p_net_open(void *ctx) { (void)ctx; return 0; }
static void p_net_close(void *ctx) { (void)ctx; }

static int wait_fd(int fd, short ev, uint64_t deadline) {
  for (;;) {
    uint64_t now = p_now(NULL);
    struct pollfd pf = {fd, ev, 0};
    int r;
    if (now >= deadline) return -ETIMEDOUT;
    r = poll(&pf, 1, (int)(deadline - now));
    if (r < 0) { if (errno == EINTR) continue; return -errno; }
    if (r > 0) return 0;
  }
}

static int p_exchange(void *ctx, const uint8_t ip[4], uint16_t port, const void *req, size_t req_len,
                      void *resp, size_t resp_cap, size_t *resp_len, uint64_t deadline) {
  struct sockaddr_in sa;
  size_t sent = 0, got = 0;
  int fd, rc = 0, err = 0;
  socklen_t el = sizeof err;
  PosixCtx *px = ctx;
  *resp_len = 0;
  if (px->override) { ip = px->oip; port = px->oport; }
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -errno;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
  { int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET; sa.sin_port = htons(port);
  memcpy(&sa.sin_addr, ip, 4);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
    if (errno != EINPROGRESS) { rc = -errno; goto out; }
    if ((rc = wait_fd(fd, POLLOUT, deadline)) != 0) goto out;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) { rc = -errno; goto out; }
    if (err) { rc = -err; goto out; }
  }
  while (sent < req_len) {
    ssize_t r = send(fd, (const char *)req + sent, req_len - sent, SEND_FLAGS);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) { rc = -errno; goto out; }
      if ((rc = wait_fd(fd, POLLOUT, deadline)) != 0) goto out;
      continue;
    }
    sent += (size_t)r;
  }
  for (;;) {
    ssize_t r;
    if (got == resp_cap) {
      char c;   /* full buffer: only a clean close is acceptable */
      r = recv(fd, &c, 1, 0);
      if (r == 0) break;
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { if ((rc = wait_fd(fd, POLLIN, deadline)) != 0) goto out; continue; }
      rc = r < 0 ? -errno : -EMSGSIZE; goto out;
    }
    r = recv(fd, (char *)resp + got, resp_cap - got, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) { rc = -errno; goto out; }
      if ((rc = wait_fd(fd, POLLIN, deadline)) != 0) goto out;
      continue;
    }
    if (r == 0) break;
    got += (size_t)r;
  }
  *resp_len = got;
out:
  close(fd);
  return rc;
}

void posix_plat_init(SyncPlat *p, PosixCtx *c, const char *root) {
  snprintf(c->root, sizeof c->root, "%s", root);
  c->override = 0;
  p->ctx = c; p->file_size = p_size; p->read_at = p_read; p->write_at = p_write; p->now_ms = p_now;
  p->net_open = p_net_open; p->net_close = p_net_close; p->exchange = p_exchange;
}
