/*
 * M4 - test_bridge_periph: the bridge window map, decode rules and the stub
 * peripheral behaviour (no sockets, no threads).
 *
 *   window decode (gpio / i2c / uart / packetizer), little-endian registers
 *   straddle rejected, outside rejected, overlapping windows rejected
 *   32-bit poke / readback round trip
 *   gpio reset register asserts reset_asserted (read-only flag register)
 *   uart FIFO push / pop order and wrap
 *   packetizer program_dest / get_dest round trips an hml_dest_cfg exactly
 */

#include "hml_bridge.h"
#include "hml_bridge_stub.h"

#include "hml_t_m4.h"

#include <string.h>

static void test_window_map(hml_bridge_t *b) {
  T_EQ_U(hml_bridge_periph_count(b), 4u, "periph count");

  uint32_t base = 0, size = 0;
  T_ST(hml_bridge_periph_info(b, "gpio", &base, &size), HML_OK, "gpio info");
  T_EQ_U(base, HML_STUB_GPIO_BASE, "gpio base");
  T_EQ_U(size, HML_STUB_GPIO_SIZE, "gpio size");
  T_ST(hml_bridge_periph_info(b, "i2c", &base, &size), HML_OK, "i2c info");
  T_EQ_U(base, HML_STUB_I2C_BASE, "i2c base");
  T_EQ_U(size, HML_STUB_I2C_SIZE, "i2c size");
  T_ST(hml_bridge_periph_info(b, "uart", &base, &size), HML_OK, "uart info");
  T_EQ_U(base, HML_STUB_UART_BASE, "uart base");
  T_EQ_U(size, HML_STUB_UART_SIZE, "uart size");
  T_ST(hml_bridge_periph_info(b, "packetizer", &base, &size), HML_OK, "pktz info");
  T_EQ_U(base, HML_STUB_PKTZ_BASE, "pktz base");
  T_EQ_U(size, HML_STUB_PKTZ_SIZE, "pktz size");
  T_ST(hml_bridge_periph_info(b, "nope", &base, &size), HML_ERR_STATE, "unknown periph info");

  /* Decode: a 32-bit poke in each register file lands in the right one. */
  uint32_t rb = 0;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x10u), 0x12345678u, &rb), HML_OK, "i2c poke");
  T_EQ_U(rb, 0x12345678u, "i2c poke readback");
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_gpio_addr(2u), 0xcafebabeu, &rb), HML_OK, "gpio poke");
  T_EQ_U(rb, 0xcafebabeu, "gpio poke readback");
  T_EQ_U(hml_bridge_stub_i2c_writes(b), 1u, "only the i2c poke hit the i2c window");

  /* Register values are little-endian on the map. */
  uint8_t raw[4];
  T_ST(hml_bridge_read(b, hml_bridge_stub_i2c_addr(0x10u), sizeof(raw), raw), HML_OK, "raw read");
  T_EQ_U(raw[0], 0x78u, "le byte 0");
  T_EQ_U(raw[1], 0x56u, "le byte 1");
  T_EQ_U(raw[2], 0x34u, "le byte 2");
  T_EQ_U(raw[3], 0x12u, "le byte 3");

  /* Byte granular access inside one window is fine. */
  T_ST(hml_bridge_read(b, hml_bridge_stub_i2c_addr(0x10u), 2u, raw), HML_OK, "2 byte read");
  T_EQ_U(raw[0], 0x78u, "2 byte read[0]");
}

static void test_straddle_and_outside(hml_bridge_t *b) {
  uint8_t buf[16];
  memset(buf, 0xaa, sizeof(buf));

  /* Runs off the end of the i2c window into the gap before uart. */
  T_ST(hml_bridge_read(b, HML_STUB_I2C_BASE + HML_STUB_I2C_SIZE - 4u, 8u, buf), HML_ERR_ARG,
       "read past the i2c window");
  for (unsigned i = 0; i < sizeof(buf); i++)
    T_CHECK(buf[i] == 0xaau, "refused read left byte %u at 0x%02x", i, buf[i]);

  /* A refused write must not apply anything either. */
  memset(buf, 0x11, sizeof(buf));
  T_ST(hml_bridge_write(b, HML_STUB_I2C_BASE + HML_STUB_I2C_SIZE - 2u, buf, 4u), HML_ERR_ARG,
       "write past the i2c window");
  uint32_t tail_lo = 1, tail_hi = 1;
  T_ST(hml_bridge_stub_i2c_get(b, HML_STUB_I2C_REGS - 2u, &tail_lo), HML_OK, "tail reg lo");
  T_ST(hml_bridge_stub_i2c_get(b, HML_STUB_I2C_REGS - 1u, &tail_hi), HML_OK, "tail reg hi");
  T_EQ_U(tail_lo, 0u, "tail reg lo untouched");
  T_EQ_U(tail_hi, 0u, "tail reg hi untouched");

  /* Starts inside uart, would run into the packetizer window: still refused. */
  memset(buf, 0x22, sizeof(buf));
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE + HML_STUB_UART_SIZE - 4u, buf, 0x110u), HML_ERR_ARG,
       "write across two peripherals");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "uart fifo untouched by refused write");
  uint32_t pk = 0xffffffffu;
  (void)hml_bridge_read32(b, hml_bridge_stub_pktz_addr(0u), &pk);
  T_EQ_U(pk, 0u, "packetizer untouched by refused write");

  /* Outside every window: above the map, in the gaps, and just past the end. */
  T_ST(hml_bridge_read(b, 0x2000u, 4u, buf), HML_ERR_ARG, "read above the map");
  T_ST(hml_bridge_write(b, 0x2000u, buf, 4u), HML_ERR_ARG, "write above the map");
  T_ST(hml_bridge_read(b, HML_STUB_GPIO_SIZE, 4u, buf), HML_ERR_ARG, "read just past gpio");
  T_ST(hml_bridge_read(b, 0x50u, 4u, buf), HML_ERR_ARG, "read in the gap after gpio");
  T_ST(hml_bridge_read(b, 0x600u - 1u, 4u, buf), HML_ERR_ARG, "read in the gap before uart");
  T_ST(hml_bridge_read(b, HML_STUB_PKTZ_BASE + HML_STUB_PKTZ_SIZE, 4u, buf), HML_ERR_ARG,
       "read just past the packetizer");

  /* Zero length transfers are refused, not silently successful. */
  T_ST(hml_bridge_read(b, HML_STUB_I2C_BASE, 0u, buf), HML_ERR_ARG, "zero length read");
  T_ST(hml_bridge_write(b, HML_STUB_I2C_BASE, buf, 0u), HML_ERR_ARG, "zero length write");
  T_ST(hml_bridge_read(b, HML_STUB_I2C_BASE, 4u, NULL), HML_ERR_ARG, "null out buffer");
  T_ST(hml_bridge_write(b, HML_STUB_I2C_BASE, NULL, 4u), HML_ERR_ARG, "null in buffer");
}

static hml_status junk_rd(void *ctx, uint32_t addr, size_t n, uint8_t *out) {
  (void)ctx;
  (void)addr;
  memset(out, 0x5a, n);
  return HML_OK;
}

static hml_status junk_wr(void *ctx, uint32_t addr, const uint8_t *in, size_t n) {
  (void)ctx;
  (void)addr;
  (void)in;
  (void)n;
  return HML_OK;
}

static void test_overlap_and_removal(hml_bridge_t *b) {
  hml_bridge_periph p;
  memset(&p, 0, sizeof(p));
  p.rd = junk_rd;
  p.wr = junk_wr;
  p.ctx = NULL;

  /* Fully inside i2c, overlapping the tail of i2c, and overlapping its head. */
  p.name = "overlap_mid";
  p.base = 0x200u;
  p.size = 0x100u;
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_STATE, "window inside another");

  p.base = HML_STUB_I2C_BASE + HML_STUB_I2C_SIZE - 0x10u;
  p.size = 0x20u;
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_STATE, "window over the tail");

  p.base = 0x0f0u;
  p.size = 0x20u;
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_STATE, "window over the head");

  /* Duplicate name is refused as well. */
  p.name = "i2c";
  p.base = 0x900u;
  p.size = 0x10u;
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_STATE, "duplicate name");

  /* A window that starts exactly where the i2c window ends is legal, and
   * decodes on its own. */
  p.name = "abuts";
  p.base = HML_STUB_I2C_BASE + HML_STUB_I2C_SIZE;
  p.size = 0x20u;
  T_ST(hml_bridge_add_periph(b, &p), HML_OK, "abutting window");
  T_EQ_U(hml_bridge_periph_count(b), 5u, "count after add");

  uint8_t buf[4];
  T_ST(hml_bridge_read(b, p.base, sizeof(buf), buf), HML_OK, "read the abutting window");
  T_EQ_U(buf[0], 0x5au, "abutting window contents");
  /* Still one window per access: the last 2 bytes of i2c plus the abutting
   * window is a straddle, not a merge. */
  T_ST(hml_bridge_read(b, p.base - 2u, sizeof(buf), buf), HML_ERR_ARG, "straddle at the boundary");

  T_ST(hml_bridge_remove_periph(b, "abuts"), HML_OK, "remove");
  T_EQ_U(hml_bridge_periph_count(b), 4u, "count after remove");
  T_ST(hml_bridge_read(b, p.base, sizeof(buf), buf), HML_ERR_ARG, "removed window is gone");
  T_ST(hml_bridge_remove_periph(b, "abuts"), HML_ERR_STATE, "remove twice");
  T_ST(hml_bridge_remove_periph(b, "nope"), HML_ERR_STATE, "remove unknown");

  /* Bad arguments. */
  p.name = "";
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_ARG, "empty name");
  static char toolong[HML_BRIDGE_NAME_MAX + 4];
  memset(toolong, 'x', sizeof(toolong) - 1u);
  p.name = toolong;
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_ARG, "name longer than HML_BRIDGE_NAME_MAX");
  p.name = "zero";
  p.size = 0;
  T_ST(hml_bridge_add_periph(b, &p), HML_ERR_ARG, "zero sized window");
}

static void test_poke_readback(hml_bridge_t *b) {
  uint32_t rb = 0xdeadbeefu;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x20u), 0xa5a5a5a5u, &rb), HML_OK, "poke");
  T_EQ_U(rb, 0xa5a5a5a5u, "poke readback");

  uint32_t v = 0;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_i2c_addr(0x20u), &v), HML_OK, "read32");
  T_EQ_U(v, 0xa5a5a5a5u, "read32 value");

  /* A refused poke leaves the readback argument alone. */
  rb = 0xfeedfaceu;
  T_ST(hml_bridge_poke32(b, 0x2000u, 1u, &rb), HML_ERR_ARG, "poke outside the map");
  T_EQ_U(rb, 0xfeedfaceu, "readback untouched by a refused poke");
  v = 0xfeedfaceu;
  T_ST(hml_bridge_read32(b, 0x2000u, &v), HML_ERR_ARG, "read32 outside the map");
  T_EQ_U(v, 0xfeedfaceu, "out untouched by a refused read32");

  /* A 4-byte poke is just a 4-byte write: it covers reg 0x21 only, and the
   * registers either side of it are untouched. */
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x21u), 0x11223344u, &rb), HML_OK, "poke reg 0x21");
  T_EQ_U(rb, 0x11223344u, "poke readback");
  T_ST(hml_bridge_stub_i2c_get(b, 0x20u, &v), HML_OK, "get reg 0x20");
  T_EQ_U(v, 0xa5a5a5a5u, "reg 0x20 untouched");
  T_ST(hml_bridge_stub_i2c_get(b, 0x21u, &v), HML_OK, "get reg 0x21");
  T_EQ_U(v, 0x11223344u, "reg 0x21 written");
  T_ST(hml_bridge_stub_i2c_get(b, 0x22u, &v), HML_OK, "get reg 0x22");
  T_EQ_U(v, 0u, "reg 0x22 untouched");

  /* The register index maps to a 4-byte register: poke/read the same word. */
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x2fu), 0x01020304u, &rb), HML_OK, "poke high reg");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_i2c_addr(0x2fu), &v), HML_OK, "read32 high reg");
  T_EQ_U(v, 0x01020304u, "high reg round trip");
}

static void test_gpio_reset(hml_bridge_t *b) {
  uint32_t v = 0xffffffffu;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET_FLAG), &v), HML_OK,
       "read reset flag before reset");
  T_EQ_U(v, 0u, "reset flag starts clear");
  T_CHECK(!hml_bridge_stub_gpio_reset_asserted(b), "reset_asserted starts false");

  uint32_t rb = 0;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET), 1u, &rb), HML_OK,
       "reset poke");
  T_EQ_U(rb, 1u, "reset readback");
  T_CHECK(hml_bridge_stub_gpio_reset_asserted(b), "reset_asserted set");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET_FLAG), &v), HML_OK,
       "read reset flag after reset");
  T_EQ_U(v, 1u, "reset flag readable at 0x04");

  /* The flag register is read-only: writing it must not clear the flag. */
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET_FLAG), 0u, &rb), HML_OK,
       "write the flag register");
  T_EQ_U(rb, 1u, "flag register still reads 1");
  T_CHECK(hml_bridge_stub_gpio_reset_asserted(b), "reset_asserted survives the write");
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET_FLAG), 0xffffffffu, &rb),
       HML_OK, "write the flag register again");
  T_EQ_U(rb, 1u, "flag register ignores writes");
}

static void test_uart_fifo(hml_bridge_t *b) {
  uint8_t out[HML_STUB_UART_FIFO];
  uint8_t in[HML_STUB_UART_FIFO];

  uint8_t one = 0x42;
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, &one, 1u), HML_OK, "uart single write");
  T_EQ_U(hml_bridge_stub_uart_count(b), 1u, "count after one push");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 1u, out), HML_OK, "uart single pop");
  T_EQ_U(out[0], 0x42u, "uart single value");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "count after one pop");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 1u, out), HML_ERR_AGAIN, "pop from an empty fifo");

  /* Fill it, in order, then drain it. */
  for (unsigned i = 0; i < HML_STUB_UART_FIFO; i++) in[i] = (uint8_t)i;
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, in, HML_STUB_UART_FIFO), HML_OK, "fill the fifo");
  T_EQ_U(hml_bridge_stub_uart_count(b), HML_STUB_UART_FIFO, "fifo is full");
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, in, 1u), HML_ERR_BUSY, "push to a full fifo");
  T_EQ_U(hml_bridge_stub_uart_count(b), HML_STUB_UART_FIFO, "a refused push appends nothing");

  memset(out, 0, sizeof(out));
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, HML_STUB_UART_FIFO, out), HML_OK, "drain the fifo");
  T_CHECK(memcmp(in, out, HML_STUB_UART_FIFO) == 0, "fifo order preserved over a full drain");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "fifo empty again");

  /* Wrap the ring: interleaved pushes and pops, each span all-or-nothing.
   * 100 in, 50 out, 150 in, 50 out, 60 in -> 210 pending, order intact. */
  uint8_t src1[100], src2[150], src3[60], big[210];
  for (unsigned i = 0; i < 100u; i++) src1[i] = (uint8_t)(0x00u + i);
  for (unsigned i = 0; i < 150u; i++) src2[i] = (uint8_t)(0x80u + i);
  for (unsigned i = 0; i < 60u; i++) src3[i] = (uint8_t)(0xc0u + i);
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, src1, sizeof(src1)), HML_OK, "push 100");
  T_EQ_U(hml_bridge_stub_uart_count(b), 100u, "100 pending");
  memset(big, 0, sizeof(big));
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 50u, big), HML_OK, "pop 50");
  T_CHECK(memcmp(src1, big, 50u) == 0, "first 50 in order");
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, src2, sizeof(src2)), HML_OK, "push 150");
  T_EQ_U(hml_bridge_stub_uart_count(b), 200u, "200 pending");
  memset(big, 0, sizeof(big));
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 50u, big), HML_OK, "pop the rest of src1");
  T_CHECK(memcmp(src1 + 50, big, 50u) == 0, "the tail of src1 in order");
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, src3, sizeof(src3)), HML_OK, "push 60 (head wraps)");
  T_EQ_U(hml_bridge_stub_uart_count(b), 210u, "210 pending across the wrap");
  memset(big, 0, sizeof(big));
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, sizeof(big), big), HML_OK, "pop 210");
  T_CHECK(memcmp(src2, big, 150u) == 0, "src2 in order");
  T_CHECK(memcmp(src3, big + 150, 60u) == 0, "src3 in order after the wrap");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "fifo drained");

  /* Back-pressure: a span that would overflow the fifo is refused whole. */
  uint8_t src200[200];
  for (unsigned i = 0; i < 200u; i++) src200[i] = (uint8_t)(0x10u + i);
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, src200, 200u), HML_OK, "push 200");
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, src200, 100u), HML_ERR_BUSY, "push 100 into 56 free");
  T_EQ_U(hml_bridge_stub_uart_count(b), 200u, "the refused push appended nothing");
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, src200, 56u), HML_OK, "push exactly what fits");
  T_EQ_U(hml_bridge_stub_uart_count(b), 256u, "fifo full");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, HML_STUB_UART_FIFO, out), HML_OK, "drain 256");
  T_EQ_U(hml_bridge_stub_uart_count(b), 0u, "fifo empty again");

  /* A short read against a short fifo is refused without popping. */
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, in, 4u), HML_OK, "push 4");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 8u, out), HML_ERR_AGAIN, "pop more than available");
  T_EQ_U(hml_bridge_stub_uart_count(b), 4u, "nothing popped by the refused read");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 4u, out), HML_OK, "pop exactly what is there");
  T_CHECK(memcmp(in, out, 4u) == 0, "the 4 bytes came back in order");
}

static void build_dest(hml_dest_cfg *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->kind = HML_XPORT_UDP;
  cfg->ipv4 = 0x7f000001u;
  cfg->udp_port = 45601u;
  cfg->psn = 0x0000abcdu;
  cfg->qp = 0x00001122u;
  cfg->rkey = 0x0000deadu;
  cfg->remote_addr = 0x00007f0000100000ull;
  cfg->frame_size = 4096u;
  cfg->window_len = 16u;
  cfg->hop_limit = 42u;
}

static void test_packetizer(hml_bridge_t *b) {
  hml_dest_cfg want, got;
  build_dest(&want);
  memset(&got, 0, sizeof(got));

  /* No destination programmed yet. */
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_ERR_STATE, "get_dest before program");

  /* The packetizer is write protected before CONFIG. */
  T_ST(hml_bridge_stub_program_dest(b, &want), HML_ERR_STATE, "program dest from INIT");
  uint32_t v = 0xffffffffu;
  (void)hml_bridge_read32(b, hml_bridge_stub_pktz_addr(0u), &v);
  T_EQ_U(v, 0u, "program refused from INIT leaves the words at zero");

  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_CONFIG), HML_OK, "enter CONFIG");
  T_ST(hml_bridge_stub_program_dest(b, &want), HML_OK, "program dest");
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_OK, "get_dest");
  T_CHECK(memcmp(&want, &got, sizeof(want)) == 0, "dest round trip is exact");

  /* Documented packing of the four words. */
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &v), HML_OK, "read ip");
  T_EQ_U(v, want.ipv4, "pktz reg 0 is the dest ipv4");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_PORT), &v), HML_OK, "read port");
  T_EQ_U(v, want.udp_port, "pktz reg 1 is the dest udp port");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_QP), &v), HML_OK, "read qp");
  T_EQ_U(v, want.qp, "pktz reg 2 is the qp");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_RKEY), &v), HML_OK, "read rkey");
  T_EQ_U(v, want.rkey, "pktz reg 3 is the rkey");

  /* get_dest reads the words out of the register file: a direct poke shows up. */
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_PORT), 0x1234u, &v), HML_OK,
       "poke the port word");
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_OK, "get_dest after the poke");
  T_EQ_U(got.udp_port, 0x1234u, "get_dest follows the register file");
  T_EQ_U(got.ipv4, want.ipv4, "get_dest keeps the other words");
  T_EQ_U(got.rkey, want.rkey, "get_dest keeps rkey");

  /* Reprogramming overwrites all four words. */
  hml_dest_cfg other;
  build_dest(&other);
  other.ipv4 = 0x0a000005u;
  other.udp_port = 4791u;
  other.qp = 0x00009999u;
  other.rkey = 0x11112222u;
  other.kind = HML_XPORT_ROCEV2;
  T_ST(hml_bridge_stub_program_dest(b, &other), HML_OK, "reprogram dest");
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_OK, "get_dest after reprogram");
  T_CHECK(memcmp(&other, &got, sizeof(other)) == 0, "second round trip is exact");
  T_EQ_U(got.kind, (unsigned)HML_XPORT_ROCEV2, "kind survives");

  T_ST(hml_bridge_stub_program_dest(b, NULL), HML_ERR_ARG, "program a null cfg");
  T_ST(hml_bridge_stub_get_dest(b, NULL), HML_ERR_ARG, "get into a null cfg");
}

int main(void) {
  hml_bridge_t *b = NULL;
  T_ST(hml_bridge_stub_hml_device(&b), HML_OK, "stub device");
  T_CHECK(b != NULL, "device handle");

  test_window_map(b);
  test_straddle_and_outside(b);
  test_overlap_and_removal(b);
  test_poke_readback(b);
  test_gpio_reset(b);
  test_uart_fifo(b);
  test_packetizer(b);

  hml_bridge_close(b);
  T_DONE("test_bridge_periph");
}
