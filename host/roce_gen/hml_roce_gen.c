#include "hml_roce_gen.h"

#include "hml/hml_crc.h"
#include "hml/hml_port.h"
#include "hml_synth_priv.h" /* deterministic synthetic pattern */
#include "hml_udp_priv.h"
#include "hml_roce_priv.h"

#include <string.h>
#include <time.h>

/*
 * Bounded retry on a full socket send buffer: hml_stream only advances its
 * counters on success, and the RoCEv2 transport leaves psn/ip_id untouched when
 * hml_sock_send fails, so re-sending the identical fragment is safe.
 */
#define HML_ROCE_GEN_TX_TRIES 2000
#define HML_ROCE_GEN_TX_SLEEP_US 200

static void gen_sleep_us(uint64_t us) {
  struct timespec ts;
  ts.tv_sec = (time_t)(us / 1000000ull);
  ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
  (void)nanosleep(&ts, NULL);
}

hml_status hml_roce_gen_xport_open(hml_roce_gen_xport *g, const hml_dest_cfg *dest,
                                   uint16_t stream_id, uint16_t src_port, bool allow_fallback) {
  if (!g || !dest) return HML_ERR_ARG;
  memset(g, 0, sizeof(*g));
  g->ops = NULL;
  g->roce_status = HML_ERR_STATE;

  hml_xport_t x;
  memset(&x, 0, sizeof(x));
  x.ops = hml_rocev2_ops();

  hml_status st = hml_roce_open_ex(&x, dest, stream_id, src_port);
  if (st == HML_OK) {
    g->xport = x;
    g->ops = x.ops;
    g->roce_status = HML_OK;
    return HML_OK;
  }
  g->roce_status = st;
  if (!allow_fallback) return st; /* nothing left open */

  /* T0 fallback: the same destination, sent as ordinary HML frame datagrams. */
  hml_dest_cfg t0 = *dest;
  t0.kind = HML_XPORT_UDP;
  memset(&x, 0, sizeof(x));
  x.ops = hml_udp_ops();

  st = x.ops->open(&x, &t0, stream_id);
  if (st != HML_OK) {
    memset(&x, 0, sizeof(x));
    return st; /* RoCEv2 and T0 both refused: a hard error, never a silent no-op */
  }
  g->xport = x;
  g->ops = x.ops;
  g->fallback = true;
  return HML_OK;
}

void hml_roce_gen_xport_close(hml_roce_gen_xport *g) {
  if (!g) return;
  if (g->ops && g->ops->close) g->ops->close(&g->xport);
  memset(&g->xport, 0, sizeof(g->xport));
  g->ops = NULL;
}

void hml_roce_gen_frame_fill(uint8_t *frame, size_t n, uint64_t frame_index, uint32_t seed) {
  if (!frame || n == 0) return;
  hml_synth_pattern_fill(frame, n, frame_index * (uint64_t)n, seed);
}

hml_status hml_roce_gen_frame_send(hml_stream_t *s, const uint8_t *frame, size_t n,
                                   size_t *chunks_out) {
  if (!s || !frame || n == 0) return HML_ERR_ARG;

  size_t off = 0;
  size_t chunks = 0;
  while (off < n) {
    size_t take = n - off;
    if (take > (size_t)HML_MAX_FRAG_PAYLOAD) take = (size_t)HML_MAX_FRAG_PAYLOAD;

    hml_status st = HML_ERR_TIMEOUT;
    for (int i = 0; i < HML_ROCE_GEN_TX_TRIES; i++) {
      st = hml_stream_send_frag(s, frame + off, take, off == 0, off + take == n, hml_clock_us());
      if (st != HML_ERR_AGAIN) break;
      gen_sleep_us(HML_ROCE_GEN_TX_SLEEP_US);
    }
    if (st != HML_OK) return st;

    off += take;
    chunks++;
  }
  if (chunks_out) *chunks_out = chunks;
  /* No EOF-only flush here: the last data fragment already carried HML_FLAG_END,
   * so the RoCEv2 transport emitted the metadata write and the T0 reassembler
   * completed the frame. A second END fragment would be a second frame, not a
   * safety net. */
  return HML_OK;
}
