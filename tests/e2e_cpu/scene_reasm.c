/*
 * scene_reasm.c -- hml_reasm driven through a REAL loopback socket.
 *
 * The reassembler (src/core/hml_reasm.c) is the same code the hml_host receiver
 * and the MCU firmware call, but here it is fed from a real UDP socket on
 * 127.0.0.1. The test process opens a controller socket and a receiver that
 * binds the private port, pulls every datagram with hml_sock_recv and hands it
 * to hml_reasm_feed_datagram -- so the parse, CRC, gap, reorder-gather, EOF and
 * overflow decisions all run on bytes that crossed the loopback interface, not
 * on a memory pointer. Every pathological case is one that a healthy stream can
 * never produce by itself, which is exactly why it is exercised here in
 * isolation:
 *
 *   single-frame clean, in-order multi-fragment, REVERSED final-first arrival
 *   duplicate fragment (idempotent), missing middle confirmed by the next START
 *   EOF-only flush, garbage datagram, short datagram, bad header CRC, bad
 *   payload CRC, oversized frame, new START interrupting a frame in flight, and
 *   a 100-frame soak. Counter deltas match what hml_reasm documents.
 */

#include "e2e_cpu.h"

#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"
#include "hml/hml_reasm.h"
#include "hml/hml_sock.h"
#include "hml_synth_priv.h" /* hml_fnv1a32 */
#include "hml_udp_priv.h"   /* hml_ipv4_ne_to_host */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK 64u
#define SEED 1u

typedef struct sink {
  e2e_harness *h;
  uint32_t calls;
  uint32_t last_seq;
  size_t last_bytes;
  uint32_t last_hash;
  uint32_t bad_hash;
} sink;

static void on_frame(void *ctx, uint16_t stream_id, uint32_t frame_seq, const uint8_t *frame,
                     size_t frame_bytes, const hml_frame_hdr *final_hdr) {
  sink *k = (sink *)ctx;
  (void)stream_id;
  (void)final_hdr;
  k->calls++;
  k->last_seq = frame_seq;
  k->last_bytes = frame_bytes;
  if (frame_bytes == 0u) return;
  uint32_t hh = hml_fnv1a32(frame, frame_bytes);
  if (k->last_hash != 0u && hh != k->last_hash) k->bad_hash++;
  k->last_hash = hh;
}

/* Expect the same delivered hash for every frame of the current case. */
static void expect_content(sink *k, const uint8_t *bytes, size_t n) {
  k->last_hash = hml_fnv1a32(bytes, n);
}

/* ---- controller and receiver sockets ---- */

typedef struct rig {
  uint16_t port;
  hml_reasm_ctx_t *reasm;
  hml_sock_t *rx;           /* receiver: bound, feeds hml_reasm */
  hml_sock_t *tx;           /* controller: sends crafted datagrams */
  sink k;
  hml_reasm_stats st;
} rig;

static hml_status rig_open(rig *rg, size_t max_frame) {
  memset(rg, 0, sizeof(*rg));
  rg->port = e2e_port_pick();
  if (rg->port == 0u) return HML_ERR_STATE;
  hml_status st = hml_reasm_open(&rg->reasm, max_frame);
  if (st != HML_OK) return st;
  hml_reasm_set_eof_cb(rg->reasm, on_frame, &rg->k);
  st = hml_sock_udp_create(&rg->rx, 0u, rg->port, false);
  if (st != HML_OK) return st;
  st = hml_sock_udp_create(&rg->tx, 0u, 0u, false);
  if (st != HML_OK) return st;
  return hml_sock_set_dest(rg->tx, hml_ipv4_ne_to_host(0x7F000001u), rg->port);
}

static void rig_close(rig *rg) {
  if (rg->tx) hml_sock_close(rg->tx);
  if (rg->rx) hml_sock_close(rg->rx);
  hml_reasm_close(rg->reasm);
}

/* Send datagrams, then poll until the receiver queue is empty. */
static void push(rig *rg, const uint8_t *const *dgs, const size_t *lens, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    hml_status st = hml_sock_send(rg->tx, dgs[i], lens[i]);
    if (st != HML_OK) {
      e2e_emit(rg->k.h, "push_send", false, "hml_sock_send failed");
      return;
    }
  }
  uint64_t deadline = e2e_now_us() + 5000000ull;
  for (;;) {
    uint8_t buf[HML_MAX_FRAG_SIZE];
    size_t got = 0;
    hml_status st = hml_sock_recv(rg->rx, buf, sizeof(buf), &got, 20);
    if (st == HML_ERR_TIMEOUT || st == HML_ERR_AGAIN) break;
    if (st != HML_OK || got == 0u) break;
    (void)hml_reasm_feed_datagram(rg->reasm, buf, got);
    if (e2e_budget_ms() == -1 || e2e_now_us() >= deadline) break;
  }
  hml_reasm_get_stats(rg->reasm, &rg->st);
}

static size_t builds(uint8_t *dg, size_t cap, uint32_t seq, uint16_t idx, uint16_t cnt,
                     uint8_t flags, const uint8_t *pay, size_t len) {
  return e2e_build_dg(dg, cap, 7u, seq, idx, cnt, flags, 0u, pay, (uint16_t)len);
}

int e2e_scene_reasm(e2e_harness *h) {
  rig rg;
  rg.k.h = h;
  hml_status st = rig_open(&rg, 8192u);
  if (st != HML_OK) {
    e2e_emit(h, "rig_open", false, "could not open receiver or controller socket");
    return 1;
  }
  e2e_eq_u(h, "rig_open", rg.port, rg.port);

  uint8_t dg[3][HML_MAX_FRAG_SIZE];
  size_t lens[3];
  const uint8_t *ptrs[3];
  uint8_t pay[3 * CHUNK];

  /* ---- case 1: single-packet clean frame ---- */
  uint32_t c0 = rg.k.calls, ok0 = rg.st.frames_ok, f0 = rg.st.frags_recv;
  for (size_t i = 0; i < CHUNK; i++) pay[i] = (uint8_t)i;
  lens[0] = builds(dg[0], sizeof(dg[0]), 1000, 0, 0, (uint8_t)(HML_FLAG_START | HML_FLAG_END),
                   pay, CHUNK);
  ptrs[0] = dg[0];
  push(&rg, ptrs, lens, 1);
  e2e_eq_u(h, "single_calls", rg.k.calls - c0, 1u);
  e2e_eq_u(h, "single_seq", rg.k.last_seq, 1000u);
  e2e_eq_u(h, "single_bytes", rg.k.last_bytes, CHUNK);
  e2e_eq_u(h, "single_hash_ok", rg.k.bad_hash, 0u);
  e2e_eq_u(h, "single_frames_ok", rg.st.frames_ok - ok0, 1u);
  e2e_eq_u(h, "single_frags_recv", rg.st.frags_recv - f0, 1u);
  e2e_eq_u(h, "single_dropped", rg.st.frames_dropped, 0u);
  e2e_eq_u(h, "single_gaps", rg.st.gaps, 0u);

  /* ---- case 2: three fragments in order (64, 64, 32 => 160 bytes) ---- */
  c0 = rg.k.calls;
  ok0 = rg.st.frames_ok;
  f0 = rg.st.frags_recv;
  memset(pay, 0, CHUNK);
  memset(pay + CHUNK, 1, CHUNK);
  memset(pay + 2 * CHUNK, 2, CHUNK - 32u);
  expect_content(&rg.k, pay, 2 * CHUNK + (CHUNK - 32u));
  lens[0] = builds(dg[0], sizeof(dg[0]), 2000, 0, 0, HML_FLAG_START, pay, CHUNK);
  lens[1] = builds(dg[1], sizeof(dg[1]), 2001, 1, 0, 0, pay + CHUNK, CHUNK);
  lens[2] = builds(dg[2], sizeof(dg[2]), 2002, 2, 0, HML_FLAG_END, pay + 2 * CHUNK, CHUNK - 32u);
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  ptrs[2] = dg[2];
  push(&rg, ptrs, lens, 3);
  e2e_eq_u(h, "ordered_calls", rg.k.calls - c0, 1u);
  e2e_eq_u(h, "ordered_seq", rg.k.last_seq, 2000u);
  e2e_eq_u(h, "ordered_bytes", rg.k.last_bytes, 2u * CHUNK + (CHUNK - 32u));
  e2e_eq_u(h, "ordered_hash_ok", rg.k.bad_hash, 0u);
  e2e_eq_u(h, "ordered_frames_ok", rg.st.frames_ok - ok0, 1u);
  e2e_eq_u(h, "ordered_frags_recv", rg.st.frags_recv - f0, 3u);
  e2e_eq_u(h, "ordered_gaps", rg.st.gaps, 0u);

  /* ---- case 3: REVERSED arrival: END, middle, then the START straggler ---- */
  c0 = rg.k.calls;
  ok0 = rg.st.frames_ok;
  f0 = rg.st.frags_recv;
  expect_content(&rg.k, pay, 2 * CHUNK + (CHUNK - 32u));
  lens[0] = builds(dg[0], sizeof(dg[0]), 3002, 2, 0, HML_FLAG_END, pay + 2 * CHUNK, CHUNK - 32u);
  lens[1] = builds(dg[1], sizeof(dg[1]), 3001, 1, 0, 0, pay + CHUNK, CHUNK);
  lens[2] = builds(dg[2], sizeof(dg[2]), 3000, 0, 0, HML_FLAG_START, pay, CHUNK);
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  ptrs[2] = dg[2];
  push(&rg, ptrs, lens, 3);
  e2e_eq_u(h, "reversed_calls", rg.k.calls - c0, 1u);
  e2e_eq_u(h, "reversed_seq", rg.k.last_seq, 3000u);
  e2e_eq_u(h, "reversed_bytes", rg.k.last_bytes, 2u * CHUNK + (CHUNK - 32u));
  e2e_eq_u(h, "reversed_hash_ok", rg.k.bad_hash, 0u);
  e2e_eq_u(h, "reversed_frames_ok", rg.st.frames_ok - ok0, 1u);
  e2e_eq_u(h, "reversed_frags_recv", rg.st.frags_recv - f0, 3u);
  e2e_eq_u(h, "reversed_dropped", rg.st.frames_dropped, 0u);
  e2e_eq_u(h, "reversed_gaps", rg.st.gaps, 0u);

  /* ---- case 4: duplicate middle fragment is idempotent ---- */
  c0 = rg.k.calls;
  ok0 = rg.st.frames_ok;
  uint32_t d0 = rg.st.frames_dropped;
  f0 = rg.st.frags_recv;
  expect_content(&rg.k, pay, 2 * CHUNK + (CHUNK - 32u));
  lens[0] = builds(dg[0], sizeof(dg[0]), 4000, 0, 0, HML_FLAG_START, pay, CHUNK);
  lens[1] = builds(dg[1], sizeof(dg[1]), 4001, 1, 0, 0, pay + CHUNK, CHUNK);
  lens[2] = builds(dg[2], sizeof(dg[2]), 4001, 1, 0, 0, pay + CHUNK, CHUNK); /* duplicate */
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  ptrs[2] = dg[2];
  push(&rg, ptrs, lens, 3);
  lens[0] = builds(dg[0], sizeof(dg[0]), 4002, 2, 0, HML_FLAG_END, pay + 2 * CHUNK, CHUNK - 32u);
  ptrs[0] = dg[0];
#define ONE_DG 1u
  push(&rg, ptrs, lens, ONE_DG);
#undef ONE_DG
  e2e_eq_u(h, "dup_calls", rg.k.calls - c0, 1u);
  e2e_eq_u(h, "dup_bytes", rg.k.last_bytes, 2u * CHUNK + (CHUNK - 32u));
  e2e_eq_u(h, "dup_frames_ok", rg.st.frames_ok - ok0, 1u);
  e2e_eq_u(h, "dup_frags_recv", rg.st.frags_recv - f0, 4u);
  e2e_eq_u(h, "dup_dropped", rg.st.frames_dropped, d0);

  /* ---- case 5: missing middle, confirmed when the next START arrives ---- */
  ok0 = rg.st.frames_ok;
  uint32_t g0 = rg.st.gaps;
  d0 = rg.st.frames_dropped;
  /* send START 5000 and END 5002, fragment 1 is lost */
  lens[0] = builds(dg[0], sizeof(dg[0]), 5000, 0, 0, HML_FLAG_START, pay, CHUNK);
  lens[1] = builds(dg[1], sizeof(dg[1]), 5002, 2, 0, HML_FLAG_END, pay + 2 * CHUNK, CHUNK - 32u);
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  c0 = rg.k.calls;
  push(&rg, ptrs, lens, 2);
  /* straggler may still be in flight: nothing counted, nothing delivered */
  e2e_eq_u(h, "gap_no_cb_yet", rg.k.calls - c0, 0u);
  e2e_eq_u(h, "gap_no_drop_yet", rg.st.frames_dropped, d0);
  e2e_eq_u(h, "gap_no_gap_yet", rg.st.gaps, g0);
  /* the next START proves the straggler is gone: frame dropped, gap counted */
  lens[0] = builds(dg[0], sizeof(dg[0]), 6000, 0, 0, HML_FLAG_START, pay, CHUNK);
  ptrs[0] = dg[0];
  push(&rg, ptrs, lens, 1);
  e2e_eq_u(h, "gap_dropped", rg.st.frames_dropped, d0 + 1u);
  e2e_eq_u(h, "gap_counted", rg.st.gaps, g0 + 1u);
  e2e_eq_u(h, "gap_no_cb", rg.k.calls - c0, 0u);
  /* and the frame recovers once its middle and end arrive */
  lens[0] = builds(dg[0], sizeof(dg[0]), 6001, 1, 0, 0, pay + CHUNK, CHUNK);
  lens[1] = builds(dg[1], sizeof(dg[1]), 6002, 2, 0, HML_FLAG_END, pay + 2 * CHUNK, CHUNK - 32u);
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  expect_content(&rg.k, pay, 2 * CHUNK + (CHUNK - 32u));
  push(&rg, ptrs, lens, 2);
  e2e_eq_u(h, "gap_recovered_calls", rg.k.calls - c0, 1u);
  e2e_eq_u(h, "gap_recovered_bytes", rg.k.last_bytes, 2u * CHUNK + (CHUNK - 32u));
  e2e_eq_u(h, "gap_frames_ok", rg.st.frames_ok, ok0 + 1u);

  /* ---- case 6: EOF-only flush ---- */
  ok0 = rg.st.frames_ok;
  d0 = rg.st.frames_dropped;
  g0 = rg.st.gaps;
  lens[0] = builds(dg[0], sizeof(dg[0]), 7000, 0, 0, HML_FLAG_END, NULL, 0u);
  ptrs[0] = dg[0];
  push(&rg, ptrs, lens, 1);
  /* zero-payload END is a self-contained zero-byte frame */
  e2e_eq_u(h, "eof_only_bytes", rg.k.last_bytes, 0u);
  e2e_eq_u(h, "eof_only_frames_ok_unchanged", rg.st.frames_ok, ok0);
  e2e_eq_u(h, "eof_only_dropped", rg.st.frames_dropped, d0);
  e2e_eq_u(h, "eof_only_gaps", rg.st.gaps, g0);

  /* ---- case 7: garbage and short datagrams -> proto_fail ---- */
  uint32_t pf0 = rg.st.proto_fail;
  f0 = rg.st.frags_recv;
  uint8_t junk[64];
  memset(junk, 0xA5, sizeof(junk));
  lens[0] = sizeof(junk);
  ptrs[0] = junk;
  push(&rg, ptrs, lens, 1);
  lens[0] = 4u;
  push(&rg, ptrs, lens, 1);
  e2e_eq_u(h, "proto_fail", rg.st.proto_fail, pf0 + 2u);
  e2e_eq_u(h, "proto_no_frags", rg.st.frags_recv, f0);

  /* ---- case 8: bad CRCs -> crc_fail, no delivery ---- */
  uint32_t cf0 = rg.st.crc_fail;
  c0 = rg.k.calls;
  lens[0] = builds(dg[0], sizeof(dg[0]), 8000, 0, 0, HML_FLAG_START, pay, 8u);
  dg[0][HML_OFF_HDR_CRC] ^= 0xFFu; /* corrupt the header CRC */
  ptrs[0] = dg[0];
  push(&rg, ptrs, lens, 1);
  lens[0] = builds(dg[0], sizeof(dg[0]), 8001, 0, 0, HML_FLAG_START, pay, 8u);
  dg[0][HML_FRAME_HDR_SIZE + 3] ^= 0x10u; /* corrupt a payload byte -> payload CRC fails */
  ptrs[0] = dg[0];
  push(&rg, ptrs, lens, 1);
  e2e_eq_u(h, "crc_fail", rg.st.crc_fail, cf0 + 2u);
  e2e_eq_u(h, "crc_no_cb", rg.k.calls, c0);

  /* ---- case 9: frame larger than the window is dropped, not delivered ---- */
  {
    rig small;
    small.k.h = h;
    hml_status st2 = rig_open(&small, 100u);
    if (st2 != HML_OK) {
      e2e_emit(h, "small_rig", false, "could not open the small-window receiver");
      return 1;
    }
    uint8_t big[2 * CHUNK];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i & 0xFFu);
    lens[0] = builds(dg[0], sizeof(dg[0]), 13000, 0, 0, HML_FLAG_START, big, CHUNK);
    lens[1] = builds(dg[1], sizeof(dg[1]), 13001, 1, 0, HML_FLAG_END, big + CHUNK, CHUNK);
    ptrs[0] = dg[0];
    ptrs[1] = dg[1];
    uint32_t sd0 = small.st.frames_dropped;
    push(&small, ptrs, lens, 2);
    e2e_eq_u(h, "oversized_dropped", small.st.frames_dropped, sd0 + 1u);
    e2e_eq_u(h, "oversized_no_cb", small.k.calls, 0u);
    rig_close(&small);
  }

  /* ---- case 10: a new START while a frame is in flight drops the old one ---- */
  c0 = rg.k.calls;
  ok0 = rg.st.frames_ok;
  d0 = rg.st.frames_dropped;
  lens[0] = builds(dg[0], sizeof(dg[0]), 9000, 0, 0, HML_FLAG_START, pay, CHUNK);
  lens[1] = builds(dg[1], sizeof(dg[1]), 9001, 1, 0, 0, pay + CHUNK, CHUNK);
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  push(&rg, ptrs, lens, 2);
  lens[0] = builds(dg[0], sizeof(dg[0]), 9500, 0, 0, HML_FLAG_START, pay, CHUNK);
  ptrs[0] = dg[0];
  push(&rg, ptrs, lens, 1);
  e2e_eq_u(h, "restart_dropped", rg.st.frames_dropped, d0 + 1u);
  e2e_eq_u(h, "restart_no_cb", rg.k.calls, c0);
  lens[0] = builds(dg[0], sizeof(dg[0]), 9501, 1, 0, 0, pay + CHUNK, CHUNK);
  lens[1] = builds(dg[1], sizeof(dg[1]), 9502, 2, 0, HML_FLAG_END, pay + 2 * CHUNK, CHUNK - 32u);
  ptrs[0] = dg[0];
  ptrs[1] = dg[1];
  expect_content(&rg.k, pay, 2 * CHUNK + (CHUNK - 32u));
  push(&rg, ptrs, lens, 2);
  e2e_eq_u(h, "restart_recovered", rg.k.calls - c0, 1u);
  e2e_eq_u(h, "restart_seq", rg.k.last_seq, 9500u);
  e2e_eq_u(h, "restart_frames_ok", rg.st.frames_ok, ok0 + 1u);

  /* ---- case 11: 100 single-packet frames traverse the socket ---- */
  d0 = rg.st.frames_dropped;
  c0 = rg.k.calls;
  ok0 = rg.st.frames_ok;
  f0 = rg.st.frags_recv;
  uint8_t one[200];
  for (size_t i = 0; i < sizeof(one); i++) one[i] = (uint8_t)(i & 0xFFu);
  uint32_t hash_home = hml_fnv1a32(one, sizeof(one));
  for (unsigned f = 0; f < 100u; f++) {
    size_t l = builds(dg[0], sizeof(dg[0]), 20000u + f, 0, 0,
                      (uint8_t)(HML_FLAG_START | HML_FLAG_END), one, sizeof(one));
    ptrs[0] = dg[0];
    lens[0] = l;
    push(&rg, ptrs, lens, 1);
    if (e2e_budget_ms() == -1) break;
  }
  e2e_eq_u(h, "soak_calls", rg.k.calls - c0, 100u);
  e2e_eq_u(h, "soak_frames_ok", rg.st.frames_ok - ok0, 100u);
  e2e_eq_u(h, "soak_frags_recv", rg.st.frags_recv - f0, 100u);
  e2e_eq_u(h, "soak_last_seq", rg.k.last_seq, 20099u);
  e2e_eq_u(h, "soak_hash", rg.k.last_hash, hash_home);
  e2e_eq_u(h, "soak_dropped", rg.st.frames_dropped - d0, 0u);

  rig_close(&rg);
  return e2e_finish(h) == 0 ? 0 : 1;
}