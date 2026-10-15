/*
 * M5 / test_ports_api.c -- one API, three ports.
 *
 * The frozen contract in include/hml/hml_sock.h and include/hml/hml_port.h is
 * the same everywhere, so this test drives the POSIX port and both RTOS port
 * stubs through the same checklist and demands identical answers where the
 * contract says there must be identical answers, and the documented difference
 * where there must not (a port without lwIP refuses with HML_ERR_NOTSUP).
 *
 * LINKING THREE PORTS INTO ONE BINARY
 * -----------------------------------
 * hml_clock_us() and hml_sock_*() have exactly one definition per binary.
 * ports/posix provides them here, and each RTOS port guards its public
 * definitions with `#if !defined(HML_PORT_POSIX)`, so this binary links cleanly.
 * To still be able to observe what the RTOS ports do, each of them also
 * exports a prefixed view of the same calls (hml_port_freertos_*, hml_port_esp_*)
 * that is always compiled and always reports that port's own behaviour. The
 * public API is never renamed; the prefixed view exists only so a host build can
 * test the stub contract.
 *
 * Expected values on this host (no FreeRTOS, no ESP-IDF, no lwIP):
 *   posix    -> real sockets
 *   freertos -> HML_ERR_NOTSUP from every socket call
 *   esp      -> HML_ERR_NOTSUP from every socket call
 */

#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_sock.h"
#include "hml/hml_crc.h"

#include "hml_t_m5.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Prefixed views exported by the RTOS ports (see the comment above). */
extern uint64_t hml_port_freertos_clock_us(void);
extern hml_status hml_port_freertos_sock_udp_create(hml_sock_t **out, uint32_t bind_ip,
                                                    uint16_t bind_port, bool broadcast);
extern hml_status hml_port_freertos_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port);
extern hml_status hml_port_freertos_sock_send(hml_sock_t *s, const void *buf, size_t n);
extern hml_status hml_port_freertos_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n,
                                              int timeout_ms);
extern void hml_port_freertos_sock_close(hml_sock_t *s);

extern uint64_t hml_port_esp_clock_us(void);
extern hml_status hml_port_esp_sock_udp_create(hml_sock_t **out, uint32_t bind_ip,
                                               uint16_t bind_port, bool broadcast);
extern hml_status hml_port_esp_sock_set_dest(hml_sock_t *s, uint32_t ip, uint16_t port);
extern hml_status hml_port_esp_sock_send(hml_sock_t *s, const void *buf, size_t n);
extern hml_status hml_port_esp_sock_recv(hml_sock_t *s, void *buf, size_t cap, size_t *n,
                                         int timeout_ms);
extern void hml_port_esp_sock_close(hml_sock_t *s);

#define LOOPBACK_IP 0x7f000001u /* host byte order, as the POSIX port expects */

#define CANARY 0xA5u
#define SMALL_CAP 16u
#define PAYLOAD_LEN 200u

/* Uniform vtable so one loop body can drive all three ports. */
typedef struct port_view {
  const char *name;
  uint64_t (*clock_us)(void);
  hml_status (*create)(hml_sock_t **out, uint32_t ip, uint16_t port, bool bcast);
  hml_status (*set_dest)(hml_sock_t *s, uint32_t ip, uint16_t port);
  hml_status (*send)(hml_sock_t *s, const void *buf, size_t n);
  hml_status (*recv)(hml_sock_t *s, void *buf, size_t cap, size_t *n, int timeout_ms);
  void (*close)(hml_sock_t *s);
  int expect_sockets; /* 1 = real sockets, 0 = stub */
} port_view;

static const port_view k_posix = {"posix",    hml_clock_us,
                                  hml_sock_udp_create, hml_sock_set_dest, hml_sock_send,
                                  hml_sock_recv,        hml_sock_close,    1};

static const port_view k_freertos = {"freertos", hml_port_freertos_clock_us,
                                     hml_port_freertos_sock_udp_create,
                                     hml_port_freertos_sock_set_dest, hml_port_freertos_sock_send,
                                     hml_port_freertos_sock_recv, hml_port_freertos_sock_close, 0};

static const port_view k_esp = {"esp",   hml_port_esp_clock_us,
                                hml_port_esp_sock_udp_create, hml_port_esp_sock_set_dest,
                                hml_port_esp_sock_send, hml_port_esp_sock_recv,
                                hml_port_esp_sock_close, 0};

static const port_view *k_views[3] = {&k_posix, &k_freertos, &k_esp};

/* ---- clock ------------------------------------------------------------- */

/* The frozen POSIX port reads CLOCK_MONOTONIC and truncates to microseconds, so
 * back-to-back calls legitimately return the same value: the contract there is
 * "never goes backwards". The two RTOS stub clocks are documented to be strictly
 * increasing (they clamp to previous+1), so this test demands the stronger
 * property where the port promises it and the achievable one where it does not.
 * It is not this test's business to make a microsecond clock fake nanoseconds. */
static int clock_is_strict(const port_view *p) { return !p->expect_sockets; }

static void test_clock_monotonic(void) {
  int v;
  for (v = 0; v < 3; v++) {
    const port_view *p = k_views[v];
    int strict = clock_is_strict(p);
    uint64_t prev = p->clock_us();
    uint64_t first = prev;
    int i;
    int ok = 1;
    T_CHECK(prev > 0, "%s: clock returned 0", p->name);
    for (i = 0; i < 1000; i++) {
      uint64_t now = p->clock_us();
      int bad = strict ? (now <= prev) : (now < prev);
      if (bad) {
        ok = 0;
        T_CHECK(0, "%s: hml_clock_us() not %s at call %d (%llu -> %llu)", p->name,
                strict ? "strictly increasing" : "monotonic", i, (unsigned long long)prev,
                (unsigned long long)now);
        break;
      }
      prev = now;
    }
    T_CHECK(ok, "%s: clock not %s over 1000 calls", p->name,
            strict ? "strictly increasing" : "monotonic");
    /* 1000 back-to-back calls cannot plausibly span a second. */
    T_CHECK(prev - first < 1000000ull, "%s: 1000 clock calls spanned %llu us", p->name,
            (unsigned long long)(prev - first));
  }
  /* The two RTOS stub views read the same host source on a host build, so they
   * must land on nearly the same number. The POSIX view is not comparable here:
   * it counts from boot, the stubs from the epoch. */
  {
    uint64_t b = k_freertos.clock_us();
    uint64_t c = k_esp.clock_us();
    uint64_t d = (b > c) ? (b - c) : (c - b);
    T_CHECK(d < 1000000ull, "the two RTOS stub clocks disagree by %llu us",
            (unsigned long long)d);
  }
}

/* ---- argument validation: identical on every port ---------------------- */

static void test_arg_validation(void) {
  int v;
  for (v = 0; v < 3; v++) {
    const port_view *p = k_views[v];
    hml_sock_t *s = NULL;
    char buf[64];
    size_t n = 12345;

    /* create() validates before it reports "no sockets here". */
    T_CHECK(p->create(NULL, LOOPBACK_IP, 0, false) == HML_ERR_ARG,
            "%s: create(NULL out) must be HML_ERR_ARG", p->name);
    n = 12345;
    T_CHECK(p->recv(NULL, buf, sizeof(buf), &n, 0) == HML_ERR_ARG,
            "%s: recv(NULL sock) must be HML_ERR_ARG", p->name);
    n = 12345;
    T_CHECK(p->recv(s, buf, sizeof(buf), &n, 0) == HML_ERR_ARG,
            "%s: recv(NULL sock) must be HML_ERR_ARG", p->name);

    if (p->expect_sockets) {
      T_CHECK(p->create(&s, LOOPBACK_IP, 0, false) == HML_OK, "%s: create failed", p->name);
      T_CHECK(s != NULL, "%s: create left the handle NULL", p->name);
    } else {
      T_CHECK(p->create(&s, LOOPBACK_IP, 0, false) == HML_ERR_NOTSUP,
              "%s: stub create must be HML_ERR_NOTSUP", p->name);
      T_CHECK(s == NULL, "%s: stub create must not hand out a handle", p->name);
    }

    /* Every remaining call takes the handle, so use the real one where we have
     * it and a non-NULL placeholder otherwise: argument validation must still
     * win over "no sockets here". */
    hml_sock_t *h = (s != NULL) ? s : (hml_sock_t *)buf;

    T_CHECK(p->set_dest(NULL, LOOPBACK_IP, 1) == HML_ERR_ARG,
            "%s: set_dest(NULL) must be HML_ERR_ARG", p->name);
    T_CHECK(p->set_dest(h, LOOPBACK_IP, 1) == (p->expect_sockets ? HML_OK : HML_ERR_NOTSUP),
            "%s: set_dest(handle) unexpected status", p->name);

    T_CHECK(p->send(NULL, buf, 4) == HML_ERR_ARG, "%s: send(NULL sock) must be HML_ERR_ARG",
            p->name);
    T_CHECK(p->send(h, NULL, 4) == HML_ERR_ARG, "%s: send(NULL buf, n>0) must be HML_ERR_ARG",
            p->name);
    T_CHECK(p->send(h, buf, 0) == (p->expect_sockets ? HML_OK : HML_ERR_NOTSUP),
            "%s: send(0 bytes) unexpected status", p->name);

    n = 12345;
    T_CHECK(p->recv(h, NULL, sizeof(buf), &n, 0) == HML_ERR_ARG,
            "%s: recv(NULL buf) must be HML_ERR_ARG", p->name);
    n = 12345;
    T_CHECK(p->recv(h, buf, sizeof(buf), NULL, 0) == HML_ERR_ARG,
            "%s: recv(NULL n) must be HML_ERR_ARG", p->name);

    /* close(NULL) is a documented no-op on every port. */
    p->close(NULL);
    T_CHECK(1, "%s: close(NULL) survived", p->name);
    p->close(s);
    T_CHECK(1, "%s: close(handle) survived", p->name);
  }
}

/* ---- the documented difference: sockets or NOTSUP ---------------------- */

static void test_socket_support(void) {
  int v;
  for (v = 0; v < 3; v++) {
    const port_view *p = k_views[v];
    hml_sock_t *s = NULL;
    hml_status st = p->create(&s, LOOPBACK_IP, 0, false);
    if (p->expect_sockets) {
      T_CHECK(st == HML_OK, "posix: create must succeed, got %d", (int)st);
      T_CHECK(p->close != NULL, "posix: close must exist");
      p->close(s);
      T_CHECK(1, "posix: create/close round trip survived");
    } else {
      T_CHECK(st == HML_ERR_NOTSUP, "%s: create must be HML_ERR_NOTSUP, got %d", p->name,
              (int)st);
      T_CHECK(s == NULL, "%s: refused create must not hand out a handle", p->name);
    }
  }
}

/* ---- no overflow past cap, on either side of the support boundary ------ */

static void build_datagram(uint8_t *dgram, size_t *len) {
  hml_frame_hdr h;
  uint8_t payload[PAYLOAD_LEN];
  size_t i;
  memset(&h, 0, sizeof(h));
  memset(payload, 0x5a, sizeof(payload));
  h.stream_id = 7;
  h.seq = 42;
  h.frag_idx = 0;
  h.frag_cnt = 1;
  h.ts_us = hml_clock_us();
  h.flags = (uint8_t)(HML_FLAG_START | HML_FLAG_END);
  h.payload_len = (uint16_t)PAYLOAD_LEN;
  hml_proto_frame_fill(&h, dgram);
  for (i = 0; i < PAYLOAD_LEN; i++) dgram[HML_OFF_PAYLOAD + i] = payload[i];
  hml_put_be16(dgram + HML_OFF_PAYLOAD + PAYLOAD_LEN, hml_crc16(payload, PAYLOAD_LEN));
  *len = HML_FRAME_HDR_SIZE + PAYLOAD_LEN + HML_TRAIL_CRC_SIZE;
}

static void test_small_cap_no_overflow(void) {
  uint8_t dgram[HML_FRAME_HDR_SIZE + PAYLOAD_LEN + HML_TRAIL_CRC_SIZE];
  size_t dlen = 0;
  build_datagram(dgram, &dlen);

  /* Receiver on an ephemeral-ish loopback port, sender to it. */
  hml_sock_t *rx = NULL;
  uint16_t rx_port = 0;
  uint16_t cand;
  for (cand = 45100; cand < 45120 && rx == NULL; cand++) {
    (void)hml_sock_udp_create(&rx, LOOPBACK_IP, cand, false);
    rx_port = cand;
  }
  T_CHECK(rx != NULL, "posix: could not bind a loopback receiver");
  if (rx == NULL) return;

  hml_sock_t *tx = NULL;
  T_CHECK(hml_sock_udp_create(&tx, LOOPBACK_IP, 0, false) == HML_OK, "posix: sender create failed");
  T_CHECK(hml_sock_set_dest(tx, LOOPBACK_IP, rx_port) == HML_OK, "posix: set_dest failed");
  T_CHECK(hml_sock_send(tx, dgram, dlen) == HML_OK, "posix: send failed");

  /* Canary-padded small buffer: the port must fill at most cap bytes. */
  uint8_t small[SMALL_CAP + 16];
  memset(small, CANARY, sizeof(small));
  size_t n = 0;
  hml_status st = hml_sock_recv(rx, small, SMALL_CAP, &n, 500);
  T_CHECK(st == HML_OK, "posix: recv of a sent datagram returned %d", (int)st);
  T_CHECK(n == SMALL_CAP, "posix: truncated recv returned n=%zu, want %u", n, SMALL_CAP);
  T_CHECK(memcmp(small, dgram, SMALL_CAP) == 0, "posix: truncated payload differs");
  {
    size_t i;
    int intact = 1;
    for (i = SMALL_CAP; i < sizeof(small); i++) {
      if (small[i] != CANARY) intact = 0;
    }
    T_CHECK(intact, "posix: recv wrote past cap (canary clobbered)");
  }

  /* Drain: nothing else was sent, so a bounded poll must report empty. */
  n = 0;
  st = hml_sock_recv(rx, small, sizeof(small), &n, 50);
  T_CHECK(st == HML_ERR_TIMEOUT || st == HML_ERR_AGAIN, "posix: empty poll returned %d",
          (int)st);
  T_CHECK(n == 0, "posix: empty poll reported n=%zu", n);

  /* Same call against the RTOS stubs: refused, n zeroed, buffer untouched. */
  int v;
  for (v = 1; v < 3; v++) {
    const port_view *p = k_views[v];
    uint8_t stub[SMALL_CAP + 16];
    memset(stub, CANARY, sizeof(stub));
    size_t sn = 999;
    hml_sock_t *fake = rx; /* a real handle: the stub must still refuse */
    st = p->recv(fake, stub, SMALL_CAP, &sn, 0);
    T_CHECK(st == HML_ERR_NOTSUP, "%s: recv on a stub must be HML_ERR_NOTSUP, got %d", p->name,
            (int)st);
    T_CHECK(sn == 0, "%s: refused recv must zero n, got %zu", p->name, sn);
    {
      size_t i;
      int intact = 1;
      for (i = 0; i < sizeof(stub); i++) {
        if (stub[i] != CANARY) intact = 0;
      }
      T_CHECK(intact, "%s: refused recv wrote into the caller's buffer", p->name);
    }
  }

  hml_sock_close(tx);
  hml_sock_close(rx);
}

int main(void) {
  test_clock_monotonic();
  test_arg_validation();
  test_socket_support();
  test_small_cap_no_overflow();
  T_DONE("test_ports_api");
}
