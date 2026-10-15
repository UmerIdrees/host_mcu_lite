#include "hml/hml_port.h"
#include "hml/hml_sock.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- clock ------------------------------------------------------------- */

uint64_t hml_clock_us(void) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* ---- udp socket -------------------------------------------------------- */

struct hml_sock {
  int fd;
  struct sockaddr_in dest;
};

static hml_status set_nonblock(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0) return HML_ERR_IO;
  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return HML_ERR_IO;
  return HML_OK;
}

hml_status hml_sock_udp_create(hml_sock_t **out, uint32_t bind_ip, uint16_t bind_port,
                               bool broadcast) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return HML_ERR_IO;

  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (broadcast) (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(bind_ip);
  a.sin_port = htons(bind_port);
  if (bind_port != 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return HML_ERR_IO;
  }

  hml_sock_t *s = (hml_sock_t *)calloc(1, sizeof(*s));
  if (!s) {
    close(fd);
    return HML_ERR_NOMEM;
  }
  s->fd = fd;
  set_nonblock(fd);
  *out = s;
  return HML_OK;
}

hml_status hml_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port) {
  if (!s) return HML_ERR_ARG;
  memset(&s->dest, 0, sizeof(s->dest));
  s->dest.sin_family = AF_INET;
  s->dest.sin_addr.s_addr = htonl(ip);
  s->dest.sin_port = htons(port);
  return HML_OK;
}

hml_status hml_sock_send(hml_sock_t *s, const void *buf, size_t n) {
  if (!s || (!buf && n)) return HML_ERR_ARG;
  ssize_t r = sendto(s->fd, buf, n, 0, (struct sockaddr *)&s->dest, sizeof(s->dest));
  if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? HML_ERR_AGAIN : HML_ERR_IO;
  if ((size_t)r != n) return HML_ERR_IO;
  return HML_OK;
}

hml_status hml_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms) {
  if (!s || !buf || !n) return HML_ERR_ARG;
  *n = 0;

  if (timeout_ms != 0) {
    struct pollfd pfd;
    pfd.fd = s->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0) return HML_ERR_TIMEOUT;
    if (pr < 0) return (errno == EINTR) ? HML_ERR_AGAIN : HML_ERR_IO;
  }

  ssize_t r = recvfrom(s->fd, buf, cap, 0, NULL, NULL);
  if (r < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return HML_ERR_AGAIN;
    if (errno == EINTR) return HML_ERR_AGAIN;
    return HML_ERR_IO;
  }
  *n = (size_t)r;
  return HML_OK;
}

void hml_sock_close(hml_sock_t *s) {
  if (!s) return;
  if (s->fd >= 0) close(s->fd);
  free(s);
}
