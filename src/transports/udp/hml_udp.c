#include "hml_udp_priv.h"

#include "hml/hml_crc.h"
#include "hml/hml_sock.h"

#include <stdlib.h>
#include <string.h>

/*
 * T0 datagram transport: one UDP datagram per logical fragment, shaped by this
 * layer (HML header re-serialized + trailing payload CRC-16). TX only.
 */
typedef struct hml_udp_impl {
  hml_sock_t *sock;
  uint8_t *dgram; /* HML_MAX_FRAG_SIZE */
  uint16_t stream_id;
  uint64_t frags_sent;
  uint64_t frags_again; /* send returned HML_ERR_AGAIN */
  uint64_t bytes_sent;
} hml_udp_impl;

static hml_status udp_open(hml_xport_t *x, const hml_dest_cfg *dest, uint16_t stream_id) {
  if (!x || !dest) return HML_ERR_ARG;
  if (dest->kind != HML_XPORT_UDP) return HML_ERR_ARG;
  if (x->impl) return HML_ERR_STATE;

  hml_udp_impl *im = (hml_udp_impl *)calloc(1, sizeof(*im));
  if (!im) return HML_ERR_NOMEM;
  im->dgram = (uint8_t *)malloc((size_t)HML_MAX_FRAG_SIZE);
  if (!im->dgram) {
    free(im);
    return HML_ERR_NOMEM;
  }

  /* bind 0.0.0.0:0 (ephemeral source port), broadcast disabled */
  hml_status st = hml_sock_udp_create(&im->sock, 0u, 0u, false);
  if (st == HML_OK) st = hml_sock_set_dest(im->sock, hml_ipv4_ne_to_host(dest->ipv4), dest->udp_port);
  if (st != HML_OK) {
    hml_sock_close(im->sock);
    free(im->dgram);
    free(im);
    return st;
  }

  im->stream_id = stream_id;
  x->impl = im;
  return HML_OK;
}

static hml_status udp_send_frag(hml_xport_t *x, const hml_frame_hdr *hdr,
                                const uint8_t *payload, size_t payload_len) {
  if (!x || !hdr) return HML_ERR_ARG;
  if (!payload && payload_len > 0) return HML_ERR_ARG;
  if (payload_len > (size_t)HML_MAX_FRAG_PAYLOAD) return HML_ERR_TOOLARGE;

  hml_udp_impl *im = (hml_udp_impl *)x->impl;
  if (!im || !im->sock) return HML_ERR_STATE;

  hml_frame_hdr h = *hdr;
  h.payload_len = (uint16_t)payload_len; /* trust the caller's length, not the header */
  hml_proto_frame_fill(&h, im->dgram);
  if (payload_len > 0) memcpy(im->dgram + HML_OFF_PAYLOAD, payload, payload_len);
  hml_put_be16(im->dgram + HML_OFF_PAYLOAD + payload_len, hml_crc16(payload, payload_len));

  size_t n = (size_t)HML_FRAME_HDR_SIZE + payload_len + (size_t)HML_TRAIL_CRC_SIZE;
  hml_status st = hml_sock_send(im->sock, im->dgram, n);
  if (st == HML_OK) {
    im->frags_sent++;
    im->bytes_sent += n;
  } else if (st == HML_ERR_AGAIN) {
    im->frags_again++;
  }
  return st;
}

static hml_status udp_flush(hml_xport_t *x) {
  if (!x) return HML_ERR_ARG;
  return ((hml_udp_impl *)x->impl) ? HML_OK : HML_ERR_STATE;
}

static void udp_close(hml_xport_t *x) {
  if (!x || !x->impl) return;
  hml_udp_impl *im = (hml_udp_impl *)x->impl;
  hml_sock_close(im->sock);
  free(im->dgram);
  free(im);
  x->impl = NULL;
}

static const hml_xport_ops g_udp_ops = {"udp", udp_open, udp_send_frag, udp_flush, udp_close};

const hml_xport_ops *hml_udp_ops(void) { return &g_udp_ops; }

hml_status hml_udp_register(void) {
  if (hml_transport_lookup("udp")) return HML_OK; /* idempotent */
  return hml_transport_register(&g_udp_ops);
}
