#ifndef HML_ROCE_DEMUX_H
#define HML_ROCE_DEMUX_H

/*
 * Host-side RoCEv2 receiver / demultiplexer -- the "software NIC" for the
 * socket-crafted RoCEv2 data plane (M3 / T1a). There is no ConnectX and no
 * libibverbs here, so this module owns the whole receive path:
 *
 *   IPv4 (IHL 5 or more) | UDP | BTH | RETH | write payload | ICRC
 *
 * Every RDMA WRITE is copied into a simulated remote memory window of
 * window_len_bytes at window_base. A frame is declared complete by a single
 * write to the dedicated meta slot at window_base + frame_size (the emitter
 * derives that address purely from hml_dest_cfg, so no out-of-band state is
 * needed). The meta slot carries a 20-byte little-endian record with the magic,
 * stream id, flags, frame seq, frame length and a CRC-32/IEEE of the frame
 * payload; the frame is delivered to the callback only if that CRC matches.
 *
 * This header deliberately shares no struct with src/transports/rocev2: a
 * receiver should only know the wire format, not the sender's internals.
 */

#include "hml/hml_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_roce_rx hml_roce_rx_t;

typedef struct hml_roce_cfg {
  uint16_t udp_port;        /* bind port; must match the emitter's dest port */
  uint64_t window_base;     /* remote VA that maps to window offset 0 */
  uint32_t window_len_bytes;/* simulated remote memory window size */
  uint32_t frame_size;      /* bytes per EOF-delimited frame; meta slot is here */
  uint32_t rkey;            /* expected RETH remote key; 0 disables the check */
  bool verify_icrc;         /* verify + enforce the trailing ICRC */
} hml_roce_cfg;

typedef struct hml_roce_stats {
  uint64_t pkts;        /* datagrams received */
  uint64_t icrc_err;    /* ICRC mismatch, packet dropped */
  uint64_t proto_err;   /* bad IP/UDP/BTH/RETH, rkey or VA, dropped */
  uint64_t writes;      /* RDMA WRITEs copied into the window */
  uint64_t bytes;       /* payload bytes copied into the window */
  uint64_t eof_frames;  /* frames delivered to the callback */
  uint64_t psn_gaps;    /* detected PSN discontinuities */
  uint64_t meta_drops;  /* meta rejected: bad magic/length or CRC mismatch */
} hml_roce_stats;

typedef void (*hml_roce_eof_cb)(void *ctx, uint32_t frame_seq, const uint8_t *frame, size_t n,
                                 uint32_t crc32);

hml_status hml_roce_rx_open(hml_roce_rx_t **out, const hml_roce_cfg *cfg, hml_roce_eof_cb cb,
                            void *ctx);

/* Process every datagram that is available. timeout_ms < 0 blocks until the
 * first datagram arrives (a negative return then means "interrupted"), 0 polls
 * and > 0 waits at most that long. Returns HML_OK once at least one datagram
 * has been handled. */
hml_status hml_roce_rx_poll(hml_roce_rx_t *r, int timeout_ms);

void hml_roce_rx_stats(const hml_roce_rx_t *r, hml_roce_stats *st);
void hml_roce_rx_close(hml_roce_rx_t *r);

#ifdef __cplusplus
}
#endif

#endif /* HML_ROCE_DEMUX_H */
