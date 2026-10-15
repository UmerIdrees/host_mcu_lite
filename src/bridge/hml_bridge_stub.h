#ifndef HML_BRIDGE_STUB_H
#define HML_BRIDGE_STUB_H

#include "hml_bridge.h"

/*
 * M4 stub peripheral set: four peripherals on one bus, i.e. the map a bridge
 * mode device would offer to a host that has no pin access.
 *
 *   gpio        0x0000 .. 0x001f   8 x u32 (register file is 32 x u32, the
 *                                     window decodes registers 0..7)
 *                                0x00 RESET     write != 0 asserts the reset
 *                                              flag
 *                                0x04 RESET_FLAG read-only, 1 while asserted
 *                                0x08..0x1c     reserved
 *   i2c         0x0100 .. 0x04ff   256 x u32 register file, offset reg * 4,
 *                                4-byte values little-endian, plus an
 *                                injectable NAK fault
 *   uart        0x0600 .. 0x06ff   256-byte circular FIFO, byte granular:
 *                                a write appends n bytes, a read pops n bytes
 *   packetizer  0x0800 .. 0x083f   4 x u32 (see hml_bridge_stub_program_dest)
 *
 * Register addresses: hml_bridge_stub_i2c_addr(reg) etc. build the map address
 * of register reg (reg index * 4 for the register files).
 */

#define HML_STUB_GPIO_BASE 0x0000u
#define HML_STUB_GPIO_SIZE 0x0020u
#define HML_STUB_GPIO_REGS 32u
#define HML_STUB_GPIO_REG_RESET 0u      /* 0x00 */
#define HML_STUB_GPIO_REG_RESET_FLAG 1u /* 0x04, read-only */

#define HML_STUB_I2C_BASE 0x0100u
#define HML_STUB_I2C_SIZE 0x0400u
#define HML_STUB_I2C_REGS 256u

#define HML_STUB_UART_BASE 0x0600u
#define HML_STUB_UART_SIZE 0x0100u
#define HML_STUB_UART_FIFO 256u

#define HML_STUB_PKTZ_BASE HML_BRIDGE_PKTZ_BASE
#define HML_STUB_PKTZ_SIZE HML_BRIDGE_PKTZ_SIZE
#define HML_STUB_PKTZ_REGS 4u

/* Register indices of the packetizer block. */
#define HML_STUB_PKTZ_REG_IP 0u   /* 0x800 dest ipv4, network byte order */
#define HML_STUB_PKTZ_REG_PORT 1u /* 0x804 dest udp port, bits 0..15 */
#define HML_STUB_PKTZ_REG_QP 2u   /* 0x808 queue pair */
#define HML_STUB_PKTZ_REG_RKEY 3u /* 0x80c remote rkey */

static HML_API inline uint32_t hml_bridge_stub_gpio_addr(uint32_t reg) {
  return HML_STUB_GPIO_BASE + reg * 4u;
}
static HML_API inline uint32_t hml_bridge_stub_i2c_addr(uint32_t reg) {
  return HML_STUB_I2C_BASE + reg * 4u;
}
static HML_API inline uint32_t hml_bridge_stub_uart_addr(void) { return HML_STUB_UART_BASE; }
static HML_API inline uint32_t hml_bridge_stub_pktz_addr(uint32_t reg) {
  return HML_STUB_PKTZ_BASE + reg * 4u;
}

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the four stub peripherals and returns the bridge. The stub state
 * lives in the bridge private slot and is released by hml_bridge_close. */
hml_status hml_bridge_stub_hml_device(hml_bridge_t **out);

/* Arm the I2C NAK fault: the first n writes to the i2c window succeed, every
 * later one returns HML_ERR_IO. n < 0 disarms and resets the counter. */
hml_status hml_bridge_stub_set_nak_after(hml_bridge_t *b, int n_writes);
uint32_t hml_bridge_stub_i2c_writes(const hml_bridge_t *b);

/* Direct register-file access for setup and for dumping what a host poke left
 * behind; does not go through the bridge decode or the NAK fault. */
hml_status hml_bridge_stub_i2c_get(hml_bridge_t *b, uint32_t reg, uint32_t *out);
hml_status hml_bridge_stub_i2c_set(hml_bridge_t *b, uint32_t reg, uint32_t val);
hml_status hml_bridge_stub_gpio_get(hml_bridge_t *b, uint32_t reg, uint32_t *out);
bool hml_bridge_stub_gpio_reset_asserted(hml_bridge_t *b);
uint32_t hml_bridge_stub_uart_count(hml_bridge_t *b);

/*
 * "Program the packetizer dest" (the HSB configure step). One 16-byte write to
 * the packetizer window, packing four u32 words little-endian:
 *   0x800  reg 0  dest ipv4, network byte order, as in hml_dest_cfg.ipv4
 *   0x804  reg 1  dest udp port in bits 0..15, bits 16..31 zero
 *   0x808  reg 2  RoCEv2 queue pair
 *   0x80c  reg 3  RoCEv2 remote rkey
 * The words the packetizer needs; the remaining hml_dest_cfg fields
 * (psn, remote_addr, frame_size, window_len, hop_limit, kind) do not fit in
 * four registers, so the stub latches a shadow copy at program time.
 *
 * hml_bridge_stub_get_dest reads the four words back out of the register file
 * and rebuilds a full hml_dest_cfg from them plus the shadow, so a round trip
 * returns every field exactly as programmed (and reflects any poke the host made
 * directly to those words).
 *
 * Both calls obey the bridge: programming from INIT returns HML_ERR_STATE, and
 * a NAK fault in the i2c window is unrelated to them.
 */
hml_status hml_bridge_stub_program_dest(hml_bridge_t *b, const hml_dest_cfg *cfg);
hml_status hml_bridge_stub_get_dest(hml_bridge_t *b, hml_dest_cfg *out);

/* One-line dump of the stub state, for lifecycle traces. */
hml_status hml_bridge_stub_dump(hml_bridge_t *b, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HML_BRIDGE_STUB_H */
