#ifndef HML_BRIDGE_H
#define HML_BRIDGE_H

#include "hml/hml_port.h"
#include "hml/hml_protocol.h"
#include "hml/hml_types.h"

/*
 * M4 - bridge mode.
 *
 * A bridge owns a set of peripherals. Every peripheral is one address window
 * into a single byte-addressable map, exactly like the register blocks an MCU
 * exposes to a host that cannot touch the pins itself: the host sends a
 * (address, length) access over a slow link and the bridge decodes it, checks
 * the phase of the device and turns it into one peripheral transaction.
 *
 * WINDOW MAP (the map built by hml_bridge_stub_hml_device; hml_bridge itself
 * is map agnostic and only enforces the packetizer guard below):
 *
 *   name         window              contents
 *   -----------  ------------------  ---------------------------------------
 *   gpio         0x0000 .. 0x001f    8 x u32 registers, offset = reg * 4
 *                                    0x00 RESET  0x04 RESET_FLAG (read-only)
 *                                    0x08..0x1c reserved
 *   i2c          0x0100 .. 0x04ff    256 x u32 register file, reg * 4,
 *                                    4-byte values little-endian
 *   uart         0x0600 .. 0x06ff    256-byte circular FIFO, byte granular
 *   packetizer   0x0800 .. 0x083f    4 x u32: DEST_IP, DEST_PORT, QP, RKEY
 *                                    0x810..0x83f reserved
 *
 * The stub gpio block models a 32 x u32 register file; its window currently
 * decodes registers 0..7, so widening the window to 0x80 exposes registers
 * 8..31 without any other change.
 *
 * DECODE RULES
 *   - one access resolves to exactly one peripheral: the one whose
 *     [base, base+size) contains addr. Windows may not overlap; an overlapping
 *     hml_bridge_add_periph is refused with HML_ERR_STATE.
 *   - all-or-nothing: the whole span is checked before anything is applied, so
 *     an access that runs off the end of a window (into the gap between two
 *     windows, or across the next peripheral) is refused with HML_ERR_ARG and
 *     leaves every peripheral untouched. The same holds for an access that
 *     starts outside every window, and for a zero length access.
 *   - 4-byte register values are little-endian on the map (hml_bridge_poke32 /
 *     hml_bridge_read32 are the only little-endian users; everything else is a
 *     plain byte copy).
 *
 * PHASE RULES (configure-then-stream, ordered)
 *   INIT   -> CONFIG -> STREAM
 *   - hml_bridge_open starts in INIT. GPIO and I2C/UART accesses are allowed in
 *     every phase (the host is allowed to poke the sensor at any time).
 *   - writes to the packetizer / dest registers, i.e. any peripheral window
 *     that overlaps [HML_BRIDGE_PKTZ_BASE, +HML_BRIDGE_PKTZ_SIZE), are refused
 *     with HML_ERR_STATE unless the phase is >= CONFIG. The registers are not
 *     touched on refusal. Reads are never guarded.
 *   - HML_BRIDGE_PHASE_STREAM is never entered by hml_bridge_set_phase(): that
 *     call returns HML_ERR_STATE. Only hml_bridge_set_streaming(true) enters
 *     it, and only from a phase >= CONFIG (a START that arrives before the
 *     device was configured is refused, so the data plane can never run
 *     against an unprogrammed packetizer).
 *   - hml_bridge_set_streaming(false) leaves STREAM for CONFIG; the
 *     configuration is kept, so a restart does not reprogram the device.
 *   - hml_bridge_set_phase(INIT) is the only way back to INIT (a device reset):
 *     afterwards the packetizer is write-protected again.
 *   - a poke while streaming is allowed on purpose (HML exposures are poked
 *     mid-stream); the value survives the stop.
 */

#define HML_BRIDGE_MAX_PERIPH 8  /* fixed capacity, no allocation per device */
#define HML_BRIDGE_NAME_MAX 32   /* includes the terminating NUL */

/* Window whose writes are phase guarded (the packetizer / dest registers). */
#define HML_BRIDGE_PKTZ_BASE 0x800u
#define HML_BRIDGE_PKTZ_SIZE 0x040u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hml_bridge hml_bridge_t;

/* Both callbacks get the window-relative address (addr - base). */
typedef hml_status (*hml_bridge_read_fn)(void *ctx, uint32_t addr, size_t n, uint8_t *out);
typedef hml_status (*hml_bridge_write_fn)(void *ctx, uint32_t addr, const uint8_t *in, size_t n);

typedef struct hml_bridge_periph {
  const char *name;  /* unique, copied by hml_bridge_add_periph */
  uint32_t base;     /* first byte of the window */
  uint32_t size;     /* bytes, must be != 0 */
  hml_bridge_read_fn rd;
  hml_bridge_write_fn wr;
  void *ctx;         /* handed back to rd/wr verbatim */
} hml_bridge_periph;

typedef enum hml_bridge_phase {
  HML_BRIDGE_PHASE_INIT = 0,   /* after hml_bridge_open, and after a reset */
  HML_BRIDGE_PHASE_CONFIG = 1, /* sensor + packetizer programmed, not running */
  HML_BRIDGE_PHASE_STREAM = 2  /* entered by hml_bridge_set_streaming(true) */
} hml_bridge_phase;

/* Little-endian helpers: the register map of the bridge is little-endian. */
static HML_API inline void hml_bridge_put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static HML_API inline uint16_t hml_bridge_get_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static HML_API inline void hml_bridge_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static HML_API inline uint32_t hml_bridge_get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

hml_status hml_bridge_open(hml_bridge_t **out);
/* HML_ERR_STATE: name already used, or the window overlaps an existing one. */
hml_status hml_bridge_add_periph(hml_bridge_t *b, const hml_bridge_periph *p);
/* HML_ERR_STATE when no peripheral carries that name. */
hml_status hml_bridge_remove_periph(hml_bridge_t *b, const char *name);
unsigned hml_bridge_periph_count(const hml_bridge_t *b);
hml_status hml_bridge_periph_info(const hml_bridge_t *b, const char *name, uint32_t *base,
                                   uint32_t *size);

/* HML_ERR_ARG when the span is empty, undecodable, or runs past the window. */
hml_status hml_bridge_read(hml_bridge_t *b, uint32_t addr, size_t n, uint8_t *out);
/* HML_ERR_STATE when the span touches the packetizer window below CONFIG. */
hml_status hml_bridge_write(hml_bridge_t *b, uint32_t addr, const uint8_t *in, size_t n);

/* Write val little-endian, then read the register back. readback may be NULL;
 * on any error it is left untouched. */
hml_status hml_bridge_poke32(hml_bridge_t *b, uint32_t addr, uint32_t val, uint32_t *readback);
hml_status hml_bridge_read32(hml_bridge_t *b, uint32_t addr, uint32_t *out);

hml_bridge_phase hml_bridge_phase_of(const hml_bridge_t *b);
hml_status hml_bridge_set_phase(hml_bridge_t *b, hml_bridge_phase phase);
const char *hml_bridge_phase_str(hml_bridge_phase phase); /* "INIT"/"CONFIG"/"STREAM" */
bool hml_bridge_is_streaming(const hml_bridge_t *b);
/* HML_ERR_STATE: entering STREAM from INIT. Always succeeds when stopping. */
hml_status hml_bridge_set_streaming(hml_bridge_t *b, bool streaming);

/* Transaction counters, for diagnostics and tests. */
uint32_t hml_bridge_stat_reads(const hml_bridge_t *b);
uint32_t hml_bridge_stat_writes(const hml_bridge_t *b);
uint32_t hml_bridge_stat_errors(const hml_bridge_t *b);

/*
 * Peripheral-owned heap state (the stubs keep their register files there).
 * hml_bridge_close runs the destructor. One private slot per bridge.
 */
hml_status hml_bridge_set_priv(hml_bridge_t *b, void *priv, void (*dtor)(void *priv));
void *hml_bridge_priv(const hml_bridge_t *b);

void hml_bridge_close(hml_bridge_t *b);

#ifdef __cplusplus
}
#endif

#endif /* HML_BRIDGE_H */
