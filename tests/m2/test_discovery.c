/*
 * M2 / test_discovery - beacon codec: exact HML_DISC_SIZE golden vector,
 * round trip, and every rejection path.
 */

#include "hml/hml_crc.h"
#include "hml/hml_discovery.h"
#include "hml/hml_port.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                           \
  do {                                             \
    if (!(cond)) {                                 \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      printf(__VA_ARGS__);                         \
      printf("\n");                                \
      g_failures++;                                \
    }                                              \
  } while (0)

/* uuid=42 caps=3 fw=0x00010000 ttl=1 ts_us=0x000001000002 crc=0xab47 */
static const uint8_t k_golden[28] = {
    0x48, 0x4d, 0x4c, 0x42, 0x01, 0x00, 0x01, 0x00,  /* magic ver flags ttl rsvd */
    0x00, 0x00, 0x00, 0x2a,                          /* uuid */
    0x00, 0x00, 0x00, 0x03,                          /* caps */
    0x00, 0x01, 0x00, 0x00,                          /* fw_ver */
    0x00, 0x00, 0x01, 0x00, 0x00, 0x02,              /* ts_us (48 bit) */
    0xab, 0x47};                                     /* crc16 over [0,26) */

static hml_beacon golden_beacon(void) {
  hml_beacon b;
  memset(&b, 0, sizeof(b));
  b.version = HML_DISC_VERSION;
  b.flags = 0;
  b.ttl = 1;
  b.uuid = 42u;
  b.caps = 3u;
  b.fw_ver = 0x00010000u;
  b.ts_us = 0x000001000002ull;
  return b;
}

static void test_golden_vector(void) {
  hml_beacon b = golden_beacon();
  uint8_t buf[HML_DISC_SIZE];
  memset(buf, 0xAA, sizeof(buf));
  hml_beacon_fill(&b, buf);

  CHECK(HML_DISC_SIZE == 28u, "HML_DISC_SIZE is %u, want 28", (unsigned)HML_DISC_SIZE);
  CHECK(sizeof(buf) == 28u, "test buffer must be exactly 28 bytes");
  CHECK(memcmp(buf, k_golden, 28) == 0, "golden beacon bytes differ");
  CHECK(hml_crc16(buf, 26) == hml_get_be16(buf + 26), "golden crc does not self-verify");

  hml_beacon out;
  CHECK(hml_beacon_parse(buf, sizeof(buf), &out) == HML_OK, "golden beacon must parse");
  CHECK(out.uuid == 42u && out.caps == 3u && out.fw_ver == 0x00010000u,
        "golden beacon fields: uuid=%u caps=%u fw=0x%08x", out.uuid, out.caps, out.fw_ver);
  CHECK(out.ttl == 1u && out.version == HML_DISC_VERSION, "golden ttl/version");
  CHECK(out.ts_us == 0x000001000002ull, "golden ts_us=%llu", (unsigned long long)out.ts_us);
}

static void test_roundtrip(void) {
  hml_beacon b;
  memset(&b, 0, sizeof(b));
  b.version = HML_DISC_VERSION;
  b.flags = 0;
  b.ttl = 1;
  b.uuid = 0xdeadbeefu;
  b.caps = HML_CAP_UDP | HML_CAP_ROCEV2 | HML_CAP_GPU;
  b.fw_ver = 0x01020304u;
  b.ts_us = 0x0000123456789ab0ull & 0x0000ffffffffffffull;

  uint8_t buf[HML_DISC_SIZE + 4];
  hml_beacon_fill(&b, buf);
  CHECK(hml_crc16(buf, 26) == hml_get_be16(buf + 26), "roundtrip crc");

  hml_beacon out;
  CHECK(hml_beacon_parse(buf, HML_DISC_SIZE, &out) == HML_OK, "roundtrip parse");
  CHECK(out.version == b.version && out.flags == b.flags && out.ttl == b.ttl, "roundtrip meta");
  CHECK(out.uuid == b.uuid, "roundtrip uuid 0x%08x want 0x%08x", out.uuid, b.uuid);
  CHECK(out.caps == b.caps, "roundtrip caps 0x%08x want 0x%08x", out.caps, b.caps);
  CHECK(out.fw_ver == b.fw_ver, "roundtrip fw 0x%08x want 0x%08x", out.fw_ver, b.fw_ver);
  CHECK(out.ts_us == b.ts_us, "roundtrip ts %llu want %llu", (unsigned long long)out.ts_us,
        (unsigned long long)b.ts_us);

  /* ts_us is encoded in 48 bits, so the top bits are dropped by design */
  hml_beacon big = b;
  big.ts_us = 0xffffffffffffull;
  hml_beacon_fill(&big, buf);
  CHECK(hml_beacon_parse(buf, HML_DISC_SIZE, &out) == HML_OK, "48 bit ts parse");
  CHECK(out.ts_us == 0xffffffffffffull, "48 bit ts round trip: %llu",
        (unsigned long long)out.ts_us);
}

static void test_rejects(void) {
  hml_beacon b = golden_beacon();
  uint8_t good[HML_DISC_SIZE];
  hml_beacon_fill(&b, good);

  hml_beacon out;
  uint8_t buf[HML_DISC_SIZE];

  memcpy(buf, good, HML_DISC_SIZE);
  buf[0] = 0x00;
  CHECK(hml_beacon_parse(buf, HML_DISC_SIZE, &out) == HML_ERR_PROTO, "bad magic must be PROTO");

  memcpy(buf, good, HML_DISC_SIZE);
  buf[3] = 0x43; /* 'HMLC' */
  CHECK(hml_beacon_parse(buf, HML_DISC_SIZE, &out) == HML_ERR_PROTO, "ctrl magic must be PROTO");

  memcpy(buf, good, HML_DISC_SIZE);
  buf[12] ^= 0x10u; /* flip a caps bit */
  CHECK(hml_beacon_parse(buf, HML_DISC_SIZE, &out) == HML_ERR_CRC, "corrupt caps must be CRC");

  memcpy(buf, good, HML_DISC_SIZE);
  buf[26] ^= 0x01u; /* corrupt the crc field */
  CHECK(hml_beacon_parse(buf, HML_DISC_SIZE, &out) == HML_ERR_CRC, "corrupt crc must be CRC");

  CHECK(hml_beacon_parse(good, HML_DISC_SIZE - 1u, &out) == HML_ERR_PROTO, "short must be PROTO");
  CHECK(hml_beacon_parse(good, HML_DISC_SIZE + 1u, &out) == HML_ERR_PROTO, "long must be PROTO");
  CHECK(hml_beacon_parse(good, 0, &out) == HML_ERR_PROTO, "empty must be PROTO");
  CHECK(hml_beacon_parse(NULL, HML_DISC_SIZE, &out) == HML_ERR_ARG, "null buf must be ARG");
  CHECK(hml_beacon_parse(good, HML_DISC_SIZE, NULL) == HML_ERR_ARG, "null out must be ARG");

  /* fill must tolerate nulls without writing anywhere */
  uint8_t guard[HML_DISC_SIZE];
  memset(guard, 0x3C, sizeof(guard));
  hml_beacon_fill(NULL, guard);
  hml_beacon_fill(&b, NULL);
  for (unsigned i = 0; i < HML_DISC_SIZE; i++)
    CHECK(guard[i] == 0x3C, "null fill touched byte %u", i);
}

int main(void) {
  test_golden_vector();
  test_roundtrip();
  test_rejects();
  if (g_failures) {
    printf("test_discovery: %d check(s) FAILED\n", g_failures);
    return 1;
  }
  printf("test_discovery: all checks passed\n");
  return 0;
}
