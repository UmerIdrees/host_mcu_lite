/*
 * hml_ctrl_host - host-side control client (M2, local mode).
 *
 * Talks to hml_ctrl_device over the loopback: discovery beacon, then the
 * scripted control steps. Exit code is 0 only when every step returned HML_OK.
 */

#include "hml_client.h"
#include "hml_control.h"

#include "hml/hml_sock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_DEFAULT_CTRL_PORT ((uint16_t)HML_CTRL_PORT)
#define HOST_DEFAULT_DISC_PORT ((uint16_t)HML_DISC_PORT)
#define HOST_DEFAULT_LOCAL_PORT 45599u

typedef struct {
  hml_client_t *cli;
  uint32_t dev_ip;         /* host byte order */
  uint32_t dest_ip;        /* host byte order */
  uint16_t dest_port;
  uint32_t frame_size;
  uint32_t window_len;
  hml_xport_kind kind;     /* HML_XPORT_UDP or HML_XPORT_ROCEV2 */
  uint32_t qp;             /* RoCEv2 QP */
  uint32_t rkey;           /* RoCEv2 rkey */
  uint64_t remote_addr;    /* RoCEv2 remote memory VA */
  uint32_t psn;            /* RoCEv2 initial PSN */
  uint8_t  hop_limit;      /* TTL / hop limit */
  uint32_t rx_ms;
  hml_sock_t *rx_sock;     /* optional data sink, bound before START */
  uint32_t rx_count;
  uint64_t rx_bytes;
  uint32_t poke_addr;
  uint32_t poke_val;
  uint32_t ping_token;
} host_t;

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [options]\n"
          "  --ip A.B.C.D        device control ip (default 127.0.0.1)\n"
          "  --ctrl-port N       device control port (default 55102)\n"
          "  --disc-port N       device beacon port (default 55100)\n"
          "  --local-port N      port this client binds, device replies here (default 45599)\n"
          "  --timeout-ms N      per-request timeout (default 1000)\n"
          "  --script LIST       comma separated steps, e.g.\n"
          "                       \"discover,set_dest,start,status,stop\"\n"
          "  --interactive       read steps from stdin, one per line\n"
          "  --dest-ip A.B.C.D   data destination ip used by set_dest (default 127.0.0.1)\n"
          "  --data-port N       data destination port used by set_dest (default 55101)\n"
          "  --transport KIND    transport kind: \"udp\" or \"rocev2\" (default udp)\n"
          "  --roce              shorthand for --transport rocev2 --data-port 4791\n"
          "  --qp N              RoCEv2 destination QP (default 0)\n"
          "  --rkey N            RoCEv2 destination rkey hex/dec (default 0)\n"
          "  --remote-addr N     RoCEv2 remote memory window VA hex/dec (default 0)\n"
          "  --psn N             RoCEv2 initial PSN (default 0)\n"
          "  --hop-limit N       RoCEv2 hop limit / TTL (default 64)\n"
          "  --frame-size N      frame_size in set_dest (default 0)\n"
          "  --window-len N      window_len units of 4KiB in set_dest (default 0)\n"
          "  --rx-ms N           duration of the \"rx\" step (default 1500)\n"
          "  --poke-addr N       address for the poke step (default 3)\n"
          "  --poke-val N        value for the poke step (default 0x1234)\n"
          "steps: discover,ping,set_dest,get_dest,get_caps,status,start,stop,reset,poke,readback,rx\n",
          argv0);
}

static bool parse_u32(const char *s, uint32_t *out) {
  if (!s || !*s) return false;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 0);
  if (!end || *end != '\0' || v > 0xffffffffu) return false;
  *out = (uint32_t)v;
  return true;
}

static bool parse_u64(const char *s, uint64_t *out) {
  if (!s || !*s) return false;
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 0);
  if (!end || *end != '\0') return false;
  *out = (uint64_t)v;
  return true;
}

static bool ip4_parse(const char *s, uint32_t *out) {
  if (!s) return false;
  uint32_t v = 0;
  const char *p = s;
  for (int i = 0; i < 4; i++) {
    if (*p < '0' || *p > '9') return false;
    uint32_t o = 0;
    while (*p >= '0' && *p <= '9') {
      o = o * 10u + (uint32_t)(*p - '0');
      if (o > 255u) return false;
      p++;
    }
    v = (v << 8) | o;
    if (i < 3) {
      if (*p != '.') return false;
      p++;
    }
  }
  if (*p != '\0') return false;
  *out = v;
  return true;
}

static const char *st_name(hml_status s) {
  switch (s) {
    case HML_OK: return "HML_OK";
    case HML_ERR_ARG: return "HML_ERR_ARG";
    case HML_ERR_NOMEM: return "HML_ERR_NOMEM";
    case HML_ERR_IO: return "HML_ERR_IO";
    case HML_ERR_PROTO: return "HML_ERR_PROTO";
    case HML_ERR_AGAIN: return "HML_ERR_AGAIN";
    case HML_ERR_TIMEOUT: return "HML_ERR_TIMEOUT";
    case HML_ERR_NOTSUP: return "HML_ERR_NOTSUP";
    case HML_ERR_BUSY: return "HML_ERR_BUSY";
    case HML_ERR_CRC: return "HML_ERR_CRC";
    case HML_ERR_SEQ: return "HML_ERR_SEQ";
    case HML_ERR_TOOLARGE: return "HML_ERR_TOOLARGE";
    case HML_ERR_STATE: return "HML_ERR_STATE";
    default: return "HML_ERR_UNKNOWN";
  }
}

/* ---- steps ------------------------------------------------------------- */

static hml_status step_discover(host_t *h) {
  uint32_t uuid = 0, caps = 0, fw = 0;
  uint64_t ts = 0;
  hml_status st = hml_client_discover(h->cli, (int)h->rx_ms, &uuid, &caps, &fw, &ts);
  if (st != HML_OK) return st;
  printf("step discover ok uuid=0x%08x caps=0x%08x fw=0x%08x ts_us=%llu\n", uuid, caps, fw,
         (unsigned long long)ts);
  return HML_OK;
}

static hml_status step_ping(host_t *h) {
  uint32_t back = 0;
  hml_status st = hml_client_ping(h->cli, h->ping_token, &back);
  if (st != HML_OK) return st;
  if (back != h->ping_token) {
    printf("step ping fail token=0x%08x echoed=0x%08x mismatch\n", h->ping_token, back);
    return HML_ERR_PROTO;
  }
  printf("step ping ok token=0x%08x\n", back);
  return HML_OK;
}

static hml_status step_set_dest(host_t *h) {
  hml_dest_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.kind = h->kind;
  cfg.ipv4 = h->dest_ip; /* hml_dest_cfg.ipv4 is network byte order */
  cfg.udp_port = h->dest_port;
  cfg.frame_size = h->frame_size;
  cfg.window_len = (uint16_t)h->window_len;
  cfg.qp = h->qp;
  cfg.rkey = h->rkey;
  cfg.remote_addr = h->remote_addr;
  cfg.psn = h->psn;
  cfg.hop_limit = h->hop_limit ? h->hop_limit : 64;
  hml_status st = hml_client_set_dest(h->cli, &cfg);
  if (st != HML_OK) return st;
  if (cfg.kind == HML_XPORT_ROCEV2) {
    printf("step set_dest ok kind=rocev2 ipv4=%u.%u.%u.%u port=%u qp=%u rkey=0x%08x remote_addr=0x%llx frame_size=%u window_len=%u\n",
           (unsigned)((h->dest_ip >> 24) & 0xffu), (unsigned)((h->dest_ip >> 16) & 0xffu),
           (unsigned)((h->dest_ip >> 8) & 0xffu), (unsigned)(h->dest_ip & 0xffu),
           (unsigned)h->dest_port, (unsigned)h->qp, (unsigned)h->rkey,
           (unsigned long long)h->remote_addr, h->frame_size, (unsigned)h->window_len);
  } else {
    printf("step set_dest ok ipv4=%u.%u.%u.%u port=%u frame_size=%u\n",
           (unsigned)((h->dest_ip >> 24) & 0xffu), (unsigned)((h->dest_ip >> 16) & 0xffu),
           (unsigned)((h->dest_ip >> 8) & 0xffu), (unsigned)(h->dest_ip & 0xffu),
           (unsigned)h->dest_port, h->frame_size);
  }
  return HML_OK;
}

static hml_status step_get_dest(host_t *h) {
  hml_dest_cfg cfg;
  hml_status st = hml_client_get_dest(h->cli, &cfg);
  if (st != HML_OK) return st;
  if (cfg.kind == HML_XPORT_ROCEV2) {
    printf("step get_dest ok kind=rocev2 ipv4=%u.%u.%u.%u port=%u qp=%u rkey=0x%08x remote_addr=0x%llx frame_size=%u window_len=%u\n",
           (unsigned)((cfg.ipv4 >> 24) & 0xffu), (unsigned)((cfg.ipv4 >> 16) & 0xffu),
           (unsigned)((cfg.ipv4 >> 8) & 0xffu), (unsigned)(cfg.ipv4 & 0xffu),
           (unsigned)cfg.udp_port, (unsigned)cfg.qp, (unsigned)cfg.rkey,
           (unsigned long long)cfg.remote_addr, cfg.frame_size, (unsigned)cfg.window_len);
  } else {
    printf("step get_dest ok kind=%u ipv4=%u.%u.%u.%u port=%u frame_size=%u\n", (unsigned)cfg.kind,
           (unsigned)((cfg.ipv4 >> 24) & 0xffu), (unsigned)((cfg.ipv4 >> 16) & 0xffu),
           (unsigned)((cfg.ipv4 >> 8) & 0xffu), (unsigned)(cfg.ipv4 & 0xffu),
           (unsigned)cfg.udp_port, cfg.frame_size);
  }
  return HML_OK;
}

static hml_status step_get_caps(host_t *h) {
  uint32_t uuid = 0, caps = 0, fw = 0;
  hml_status st = hml_client_get_caps(h->cli, &uuid, &caps, &fw);
  if (st != HML_OK) return st;
  printf("step get_caps ok uuid=0x%08x caps=0x%08x fw=0x%08x\n", uuid, caps, fw);
  return HML_OK;
}

static hml_status step_status(host_t *h) {
  bool streaming = false;
  hml_status st = hml_client_status(h->cli, &streaming);
  if (st != HML_OK) return st;
  printf("step status ok streaming=%d\n", streaming ? 1 : 0);
  return HML_OK;
}

static hml_status step_start(host_t *h) {
  (void)h;
  hml_status st = hml_client_start(h->cli);
  if (st != HML_OK) return st;
  printf("step start ok\n");
  return HML_OK;
}

static hml_status step_stop(host_t *h) {
  (void)h;
  hml_status st = hml_client_stop(h->cli);
  if (st != HML_OK) return st;
  printf("step stop ok\n");
  return HML_OK;
}

static hml_status step_reset(host_t *h) {
  (void)h;
  hml_status st = hml_client_reset(h->cli);
  if (st != HML_OK) return st;
  printf("step reset ok\n");
  return HML_OK;
}

static hml_status step_poke(host_t *h) {
  uint32_t rval = 0;
  hml_status st = hml_client_poke(h->cli, h->poke_addr, h->poke_val, &rval);
  if (st != HML_OK) {
    if (st == HML_ERR_NOTSUP)
      printf("step poke fail addr=0x%08x device_status=%s\n", h->poke_addr,
             st_name(hml_client_last_device_status(h->cli)));
    return st;
  }
  printf("step poke ok addr=0x%08x val=0x%08x readback=0x%08x\n", h->poke_addr, h->poke_val, rval);
  return HML_OK;
}

static hml_status step_readback(host_t *h) {
  uint32_t val = 0;
  hml_status st = hml_client_readback(h->cli, h->poke_addr, &val);
  if (st != HML_OK) return st;
  printf("step readback ok addr=0x%08x val=0x%08x\n", h->poke_addr, val);
  return HML_OK;
}

/* Waits rx_ms on the pre-bound data socket and reports what arrived. */
static hml_status step_rx(host_t *h) {
  if (!h->rx_sock) {
    printf("step rx fail no socket bound (data port busy?)\n");
    return HML_ERR_IO;
  }
  h->rx_count = 0;
  h->rx_bytes = 0;
  uint64_t t0 = hml_clock_us();
  uint64_t budget = (uint64_t)h->rx_ms * 1000ull;
  uint8_t buf[2048];
  for (;;) {
    uint64_t elapsed = hml_clock_us() - t0;
    if (elapsed >= budget) break;
    int wait = (int)((budget - elapsed) / 1000ull);
    if (wait <= 0) wait = 1;
    size_t n = 0;
    hml_status st = hml_sock_recv(h->rx_sock, buf, sizeof(buf), &n, wait);
    if (st == HML_OK) {
      h->rx_count++;
      h->rx_bytes += n;
    } else if (st != HML_ERR_TIMEOUT && st != HML_ERR_AGAIN) {
      return st;
    }
  }
  printf("step rx ok datagrams=%u bytes=%llu\n", h->rx_count, (unsigned long long)h->rx_bytes);
  return h->rx_count > 0 ? HML_OK : HML_ERR_TIMEOUT;
}

static hml_status run_step(host_t *h, const char *name) {
  if (strcmp(name, "discover") == 0) return step_discover(h);
  if (strcmp(name, "ping") == 0) return step_ping(h);
  if (strcmp(name, "set_dest") == 0) return step_set_dest(h);
  if (strcmp(name, "get_dest") == 0) return step_get_dest(h);
  if (strcmp(name, "get_caps") == 0) return step_get_caps(h);
  if (strcmp(name, "caps") == 0) return step_get_caps(h);
  if (strcmp(name, "status") == 0) return step_status(h);
  if (strcmp(name, "start") == 0) return step_start(h);
  if (strcmp(name, "stop") == 0) return step_stop(h);
  if (strcmp(name, "reset") == 0) return step_reset(h);
  if (strcmp(name, "poke") == 0) return step_poke(h);
  if (strcmp(name, "readback") == 0) return step_readback(h);
  if (strcmp(name, "rx") == 0) return step_rx(h);
  fprintf(stderr, "unknown step '%s'\n", name);
  return HML_ERR_ARG;
}

/* Runs a comma separated step list; returns the first failure. */
static hml_status run_script(host_t *h, const char *script) {
  char buf[512];
  if (strlen(script) >= sizeof(buf)) return HML_ERR_ARG;
  strcpy(buf, script);

  hml_status worst = HML_OK;
  char *save = NULL;
  char *tok = strtok_r(buf, ",", &save);
  while (tok) {
    while (*tok == ' ') tok++;
    size_t l = strlen(tok);
    while (l > 0 && tok[l - 1] == ' ') tok[--l] = '\0';
    if (*tok) {
      hml_status st = run_step(h, tok);
      if (st != HML_OK) {
        printf("step %s fail status=%d (%s)\n", tok, (int)st, st_name(st));
        if (worst == HML_OK) worst = st;
      }
    }
    tok = strtok_r(NULL, ",", &save);
  }
  return worst;
}

static bool script_has(const char *script, const char *step) {
  if (!script) return false;
  size_t n = strlen(step);
  for (const char *p = script; (p = strstr(p, step)) != NULL; p += n) {
    bool left = (p == script) || p[-1] == ',' || p[-1] == ' ';
    bool right = p[n] == ',' || p[n] == '\0' || p[n] == ' ';
    if (left && right) return true;
  }
  return false;
}

int main(int argc, char **argv) {
  host_t h;
  memset(&h, 0, sizeof(h));
  h.dev_ip = HML_IP_LOOPBACK;
  h.dest_ip = HML_IP_LOOPBACK;
  h.dest_port = (uint16_t)HML_DATA_PORT;
  h.rx_ms = 1500;
  h.ping_token = 0xfeedbeefu;
  h.poke_addr = 3;
  h.poke_val = 0x1234u;

  uint16_t ctrl_port = HOST_DEFAULT_CTRL_PORT;
  uint16_t disc_port = HOST_DEFAULT_DISC_PORT;
  uint16_t local_port = (uint16_t)HOST_DEFAULT_LOCAL_PORT;
  uint32_t timeout_ms = HML_CTRL_DEFAULT_TIMEOUT_MS;
  const char *script = NULL;
  bool interactive = false;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    bool has_val = (i + 1) < argc;
#define NEED_VAL()                                  \
  do {                                              \
    if (!has_val) {                                 \
      fprintf(stderr, "missing value for %s\n", a); \
      return 2;                                     \
    }                                               \
  } while (0)
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(a, "--interactive") == 0) {
      interactive = true;
    } else if (strcmp(a, "--script") == 0) {
      NEED_VAL();
      script = argv[++i];
    } else if (strcmp(a, "--ip") == 0) {
      NEED_VAL();
      if (!ip4_parse(argv[++i], &h.dev_ip)) {
        fprintf(stderr, "bad --ip\n");
        return 2;
      }
    } else if (strcmp(a, "--ctrl-port") == 0) {
      NEED_VAL();
      ctrl_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--disc-port") == 0) {
      NEED_VAL();
      disc_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--local-port") == 0) {
      NEED_VAL();
      local_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--timeout-ms") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &timeout_ms)) return 2;
    } else if (strcmp(a, "--dest-ip") == 0) {
      NEED_VAL();
      if (!ip4_parse(argv[++i], &h.dest_ip)) {
        fprintf(stderr, "bad --dest-ip\n");
        return 2;
      }
    } else if (strcmp(a, "--data-port") == 0) {
      NEED_VAL();
      h.dest_port = (uint16_t)strtoul(argv[++i], NULL, 0);
    } else if (strcmp(a, "--transport") == 0) {
      NEED_VAL();
      const char *t = argv[++i];
      if (strcmp(t, "rocev2") == 0 || strcmp(t, "roce") == 0) {
        h.kind = HML_XPORT_ROCEV2;
        if (h.dest_port == (uint16_t)HML_DATA_PORT) h.dest_port = (uint16_t)HML_ROCE_PORT;
      } else if (strcmp(t, "udp") == 0) {
        h.kind = HML_XPORT_UDP;
      } else {
        fprintf(stderr, "bad --transport '%s'\n", t);
        return 2;
      }
    } else if (strcmp(a, "--roce") == 0 || strcmp(a, "--rocev2") == 0) {
      h.kind = HML_XPORT_ROCEV2;
      if (h.dest_port == (uint16_t)HML_DATA_PORT) h.dest_port = (uint16_t)HML_ROCE_PORT;
    } else if (strcmp(a, "--qp") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.qp)) return 2;
    } else if (strcmp(a, "--rkey") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.rkey)) return 2;
    } else if (strcmp(a, "--remote-addr") == 0) {
      NEED_VAL();
      if (!parse_u64(argv[++i], &h.remote_addr)) return 2;
    } else if (strcmp(a, "--psn") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.psn)) return 2;
    } else if (strcmp(a, "--hop-limit") == 0) {
      NEED_VAL();
      uint32_t hl = 0;
      if (!parse_u32(argv[++i], &hl) || hl > 255) return 2;
      h.hop_limit = (uint8_t)hl;
    } else if (strcmp(a, "--frame-size") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.frame_size)) return 2;
    } else if (strcmp(a, "--window-len") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.window_len)) return 2;
    } else if (strcmp(a, "--rx-ms") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.rx_ms)) return 2;
    } else if (strcmp(a, "--poke-addr") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.poke_addr)) return 2;
    } else if (strcmp(a, "--poke-val") == 0) {
      NEED_VAL();
      if (!parse_u32(argv[++i], &h.poke_val)) return 2;
    } else {
      fprintf(stderr, "unknown option %s\n", a);
      usage(argv[0]);
      return 2;
    }
#undef NEED_VAL
  }

  if (!script && !interactive) {
    script = "discover,set_dest,start,status,stop";
  }

  hml_status st = hml_client_open(&h.cli, local_port);
  if (st != HML_OK) {
    fprintf(stderr, "hml_client_open failed: %d\n", (int)st);
    return 1;
  }
  (void)hml_client_set_timeout(h.cli, timeout_ms);
  st = hml_client_set_peer(h.cli, h.dev_ip, ctrl_port, disc_port);
  if (st != HML_OK) {
    fprintf(stderr, "hml_client_set_peer failed: %d (is disc port %u already bound?)\n", (int)st,
            (unsigned)disc_port);
    hml_client_close(h.cli);
    return 1;
  }

  /* Bind the data sink before START so no early datagram is missed. */
  if (script_has(script, "rx")) {
    if (hml_sock_udp_create(&h.rx_sock, HML_IP_ANY, h.dest_port, false) != HML_OK) {
      fprintf(stderr, "cannot bind data port %u for rx step\n", (unsigned)h.dest_port);
      hml_client_close(h.cli);
      return 1;
    }
  }

  printf("host up local=%u peer=%u.%u.%u.%u ctrl=%u disc=%u timeout=%ums\n", (unsigned)local_port,
         (unsigned)((h.dev_ip >> 24) & 0xffu), (unsigned)((h.dev_ip >> 16) & 0xffu),
         (unsigned)((h.dev_ip >> 8) & 0xffu), (unsigned)(h.dev_ip & 0xffu),
         (unsigned)ctrl_port, (unsigned)disc_port, (unsigned)timeout_ms);
  fflush(stdout);

  hml_status worst = HML_OK;
  if (script) {
    printf("script: %s\n", script);
    fflush(stdout);
    worst = run_script(&h, script);
  }
  if (interactive && worst == HML_OK) {
    char line[256];
    printf("interactive: one step per line, 'quit' to exit\n");
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin)) {
      size_t l = strlen(line);
      while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
      if (line[0] == '\0') continue;
      if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;
      hml_status s = run_step(&h, line);
      if (s != HML_OK) {
        printf("step %s fail status=%d (%s)\n", line, (int)s, st_name(s));
        if (worst == HML_OK) worst = s;
      }
      fflush(stdout);
    }
  }

  if (worst == HML_OK) printf("host done: all steps ok\n");
  else printf("host done: first failure %s\n", st_name(worst));
  fflush(stdout);

  if (h.rx_sock) hml_sock_close(h.rx_sock);
  hml_client_close(h.cli);
  return worst == HML_OK ? 0 : 1;
}
