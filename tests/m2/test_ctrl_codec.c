/*
 * M2 / test_ctrl_codec - control message codec: golden header vector, round
 * trip, and every documented rejection path.
 */

#include "hml/hml_ctrl.h"
#include "hml/hml_crc.h"
#include "hml/hml_port.h"

#include <stdio.h>
#include <string.h>

#define CTRL_HDR_SIZE 12u /* fixed control header size, see src/control/hml_ctrl_codec.c */

static int g_failures = 0;

#define CHECK(cond, ...)                        \
  do {                                          \
    if (!(cond)) {                              \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                      \
      printf("\n");                             \
      g_failures++;                             \
    }                                           \
  } while (0)

/* op=HML_OP_PING, flags=HML_CTRL_FLAG_REQ|HML_CTRL_FLAG_ACK, len=0 */
static const uint8_t k_golden_hdr[12] = {0x48, 0x4d, 0x4c, 0x43, 0x01, 0x01,
                                         0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
static const uint8_t k_golden_crc[2] = {0x16, 0x58};

static void test_golden_header(void) {
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_PING;
  m.flags = (uint16_t)(HML_CTRL_FLAG_REQ | HML_CTRL_FLAG_ACK);
  m.payload = NULL;
  m.payload_len = 0;

  uint8_t buf[64];
  memset(buf, 0xAA, sizeof(buf));
  uint16_t n = 0;
  hml_ctrl_fill(&m, buf, (uint16_t)sizeof(buf), &n);

  CHECK(n == 14u, "header vector: total size %u, want 14", (unsigned)n);
  if (n == 14u) {
    CHECK(memcmp(buf, k_golden_hdr, 12) == 0, "header vector: 12 header bytes differ");
    CHECK(buf[12] == k_golden_crc[0] && buf[13] == k_golden_crc[1],
          "header vector: crc = %02x%02x, want 1658", buf[12], buf[13]);
    CHECK(hml_crc16(buf, 12) == hml_get_be16(buf + 12), "header vector: crc does not self-verify");
  }
}

static void test_roundtrip(void) {
  uint8_t payload[HML_CFG_SIZE];
  for (unsigned i = 0; i < HML_CFG_SIZE; i++) payload[i] = (uint8_t)(i * 3u + 1u);

  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_SET_DEST;
  m.flags = HML_CTRL_FLAG_REQ;
  m.payload = payload;
  m.payload_len = (uint16_t)HML_CFG_SIZE;

  uint8_t buf[CTRL_HDR_SIZE + HML_CTRL_MAX_PAYLOAD + 2u];
  uint16_t n = 0;
  hml_ctrl_fill(&m, buf, (uint16_t)sizeof(buf), &n);
  CHECK(n == CTRL_HDR_SIZE + HML_CFG_SIZE + 2u, "roundtrip: n=%u", (unsigned)n);

  hml_ctrl_msg out;
  const uint8_t *pl = NULL;
  hml_status st = hml_ctrl_parse(buf, n, &out, &pl);
  CHECK(st == HML_OK, "roundtrip: parse status %d", (int)st);
  CHECK(out.op == HML_OP_SET_DEST, "roundtrip: op 0x%02x", (unsigned)out.op);
  CHECK(out.flags == HML_CTRL_FLAG_REQ, "roundtrip: flags 0x%04x", (unsigned)out.flags);
  CHECK(out.payload_len == HML_CFG_SIZE, "roundtrip: payload_len %u", (unsigned)out.payload_len);
  CHECK(pl == buf + CTRL_HDR_SIZE, "roundtrip: payload must point into the input buffer");
  CHECK(out.payload == pl, "roundtrip: msg.payload must alias the payload out-param");
  if (pl) CHECK(memcmp(pl, payload, HML_CFG_SIZE) == 0, "roundtrip: payload bytes differ");

  /* ping echo with a short payload */
  hml_ctrl_msg p;
  memset(&p, 0, sizeof(p));
  p.op = HML_OP_PING;
  p.flags = HML_CTRL_FLAG_REQ;
  p.payload = payload;
  p.payload_len = 4;
  uint16_t n2 = 0;
  hml_ctrl_fill(&p, buf, (uint16_t)sizeof(buf), &n2);
  CHECK(n2 == 18u, "ping fill: n=%u want 18", (unsigned)n2);
  st = hml_ctrl_parse(buf, n2, &out, &pl);
  CHECK(st == HML_OK, "ping parse: %d", (int)st);
  CHECK(out.payload_len == 4 && pl && memcmp(pl, payload, 4) == 0, "ping payload echo");
}

static void test_rejects(void) {
  uint8_t payload[4] = {0xde, 0xad, 0xbe, 0xef};
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_SET_DEST;
  m.flags = HML_CTRL_FLAG_REQ;
  m.payload = payload;
  m.payload_len = 4;

  uint8_t good[32];
  uint16_t n = 0;
  hml_ctrl_fill(&m, good, (uint16_t)sizeof(good), &n);
  CHECK(n == 18u, "reject fixture size %u", (unsigned)n);

  hml_ctrl_msg out;
  const uint8_t *pl = NULL;

  uint8_t b[32];
  memcpy(b, good, n);
  b[0] = 0x00;
  CHECK(hml_ctrl_parse(b, n, &out, &pl) == HML_ERR_PROTO, "bad magic must be PROTO");

  memcpy(b, good, n);
  b[4] = 0x7f;
  CHECK(hml_ctrl_parse(b, n, &out, &pl) == HML_ERR_PROTO, "bad version must be PROTO");

  memcpy(b, good, n);
  hml_put_be16(b + 8, (uint16_t)(HML_CTRL_MAX_PAYLOAD + 1u));
  CHECK(hml_ctrl_parse(b, n, &out, &pl) == HML_ERR_PROTO, "len > HML_CTRL_MAX_PAYLOAD must be PROTO");

  memcpy(b, good, n);
  hml_put_be16(b + 8, (uint16_t)(HML_CTRL_MAX_PAYLOAD));
  CHECK(hml_ctrl_parse(b, n, &out, &pl) == HML_ERR_PROTO, "oversized len vs buffer must be PROTO");

  CHECK(hml_ctrl_parse(good, n - 1u, &out, &pl) == HML_ERR_PROTO, "truncated buffer must be PROTO");
  CHECK(hml_ctrl_parse(good, CTRL_HDR_SIZE, &out, &pl) == HML_ERR_PROTO,
        "header-only buffer must be PROTO");
  CHECK(hml_ctrl_parse(good, 3, &out, &pl) == HML_ERR_PROTO, "tiny buffer must be PROTO");
  CHECK(hml_ctrl_parse(good, n + 1u, &out, &pl) == HML_ERR_PROTO, "trailing junk must be PROTO");

  memcpy(b, good, n);
  b[12] ^= 0x01u; /* corrupt a payload byte, CRC now mismatches */
  CHECK(hml_ctrl_parse(b, n, &out, &pl) == HML_ERR_CRC, "corrupted payload must be CRC");

  memcpy(b, good, n);
  b[n - 1] ^= 0x80u; /* corrupt the CRC field itself */
  CHECK(hml_ctrl_parse(b, n, &out, &pl) == HML_ERR_CRC, "corrupted crc must be CRC");

  CHECK(hml_ctrl_parse(NULL, n, &out, &pl) == HML_ERR_ARG, "null buffer must be ARG");
  CHECK(hml_ctrl_parse(good, n, NULL, &pl) == HML_ERR_ARG, "null out must be ARG");
  CHECK(hml_ctrl_parse(good, n, &out, NULL) == HML_ERR_ARG, "null payload must be ARG");
}

static void test_fill_failsafe(void) {
  uint8_t payload[4] = {1, 2, 3, 4};
  hml_ctrl_msg m;
  memset(&m, 0, sizeof(m));
  m.op = HML_OP_POKE;
  m.flags = HML_CTRL_FLAG_REQ;
  m.payload = payload;
  m.payload_len = 4;
  const uint16_t need = 18u;

  uint8_t buf[32];
  memset(buf, 0xAA, sizeof(buf));
  uint16_t n = 0xFFFFu;
  hml_ctrl_fill(&m, buf, (uint16_t)(need - 1u), &n);
  CHECK(n == 0, "too-small max_dst: out_n=%u want 0", (unsigned)n);
  for (unsigned i = 0; i < need; i++)
    CHECK(buf[i] == 0xAA, "too-small max_dst: wrote %u bytes at %u", (unsigned)buf[i], i);

  memset(buf, 0xAA, sizeof(buf));
  n = 0xFFFFu;
  hml_ctrl_fill(&m, buf, 0, &n);
  CHECK(n == 0, "max_dst=0: out_n=%u want 0", (unsigned)n);
  CHECK(buf[0] == 0xAA, "max_dst=0: buffer touched");

  memset(buf, 0xAA, sizeof(buf));
  n = 0xFFFFu;
  hml_ctrl_fill(&m, buf, (uint16_t)sizeof(buf), &n);
  CHECK(n == need, "exact-fit max_dst: out_n=%u want %u", (unsigned)n, (unsigned)need);

  /* payload_len above the hard cap must never emit anything */
  uint8_t big[HML_CTRL_MAX_PAYLOAD + 1u];
  memset(big, 0x5A, sizeof(big));
  m.payload = big;
  m.payload_len = (uint16_t)(HML_CTRL_MAX_PAYLOAD + 1u);
  memset(buf, 0xAA, sizeof(buf));
  n = 0xFFFFu;
  hml_ctrl_fill(&m, buf, (uint16_t)sizeof(buf), &n);
  CHECK(n == 0, "payload_len > max: out_n=%u want 0", (unsigned)n);
  CHECK(buf[0] == 0xAA, "payload_len > max: buffer touched");

  /* null arguments must not crash and must still zero out_n */
  n = 0xFFFFu;
  hml_ctrl_fill(NULL, buf, (uint16_t)sizeof(buf), &n);
  CHECK(n == 0, "null msg: out_n=%u want 0", (unsigned)n);
  m.payload = big;
  m.payload_len = 4;
  n = 0xFFFFu;
  hml_ctrl_fill(&m, NULL, (uint16_t)sizeof(buf), &n);
  CHECK(n == 0, "null dst: out_n=%u want 0", (unsigned)n);
  hml_ctrl_fill(&m, buf, (uint16_t)sizeof(buf), NULL); /* must not crash */
}

int main(void) {
  test_golden_header();
  test_roundtrip();
  test_rejects();
  test_fill_failsafe();
  if (g_failures) {
    printf("test_ctrl_codec: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  printf("test_ctrl_codec: all checks passed\n");
  return 0;
}
