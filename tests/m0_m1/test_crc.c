/* M0: CRC check vectors. */
#include "hml/hml_crc.h"
#include "hml_test_util.h"

int main(void) {
  const char *v = "123456789";
  char a[9];

  CHECK_EQ_U32(hml_crc16(v, 9), 0x29B1u);
  CHECK_EQ_U32(hml_crc32_ieee(v, 9), 0xCBF43926u);

  /* empty-buffer determinism: documented init values, no data */
  CHECK_EQ_U32(hml_crc16(NULL, 0), 0xFFFFu);
  CHECK_EQ_U32(hml_crc32_ieee(NULL, 0), 0x00000000u);
  CHECK_EQ_U32(hml_crc16(v, 0), 0xFFFFu);
  CHECK(hml_crc16(v, 0) == hml_crc16(v, 0));
  CHECK(hml_crc32_ieee(v, 0) == hml_crc32_ieee(v, 0));

  /* single bit sensitivity */
  memcpy(a, v, 9);
  uint16_t c16 = hml_crc16(a, 9);
  a[4] = 'X';
  CHECK(hml_crc16(a, 9) != c16);

  uint32_t c32 = hml_crc32_ieee(v, 9);
  memcpy(a, v, 9);
  a[0] = '0';
  CHECK(hml_crc32_ieee(a, 9) != c32);

  /* longer buffer is a different value (length participates) */
  CHECK(hml_crc16(v, 8) != hml_crc16(v, 9));
  CHECK(hml_crc32_ieee(v, 8) != hml_crc32_ieee(v, 9));

  TEST_DONE();
}
