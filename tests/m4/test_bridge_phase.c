/*
 * M4 - test_bridge_phase: the configure-then-stream phase machine.
 *
 *   open starts in INIT, GPIO / I2C / UART accessible in every phase
 *   packetizer writes refused below CONFIG, reads never guarded
 *   set_phase(STREAM) refused; only set_streaming(true) enters STREAM
 *   a START before CONFIG is refused
 *   stop returns to CONFIG and keeps the configuration
 *   leaving STREAM with set_phase is refused; set_phase(INIT) resets
 *   mid-stream pokes are allowed and the value survives the stop
 */

#include "hml_bridge.h"
#include "hml_bridge_stub.h"

#include "hml_t_m4.h"

#include <string.h>

static void test_init(hml_bridge_t *b) {
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "open starts in INIT");
  T_CHECK(!hml_bridge_is_streaming(b), "not streaming at open");

  T_CHECK(strcmp(hml_bridge_phase_str(HML_BRIDGE_PHASE_INIT), "INIT") == 0, "phase_str INIT");
  T_CHECK(strcmp(hml_bridge_phase_str(HML_BRIDGE_PHASE_CONFIG), "CONFIG") == 0, "phase_str CONFIG");
  T_CHECK(strcmp(hml_bridge_phase_str(HML_BRIDGE_PHASE_STREAM), "STREAM") == 0, "phase_str STREAM");

  /* GPIO, I2C and UART are accessible from INIT. */
  uint32_t rb = 0;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET), 1u, &rb), HML_OK,
       "gpio poke from INIT");
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x01u), 0x01020304u, &rb), HML_OK,
       "i2c poke from INIT");
  uint8_t byte = 0x99, got = 0;
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, &byte, 1u), HML_OK, "uart push from INIT");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, 1u, &got), HML_OK, "uart pop from INIT");
  T_EQ_U(got, 0x99u, "uart value from INIT");
}

static void test_pktz_guard_in_init(hml_bridge_t *b) {
  /* Reads of the packetizer words are never guarded. */
  uint32_t v = 0xffffffffu;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &v), HML_OK,
       "pktz read allowed in INIT");
  T_EQ_U(v, 0u, "pktz reads as zero");

  /* Writes are refused in INIT, through every entry point. */
  uint32_t rb = 0xdeadbeefu;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), 0x7f000001u, &rb),
       HML_ERR_STATE, "pktz poke refused in INIT");
  T_EQ_U(rb, 0xdeadbeefu, "refused poke leaves the readback alone");

  uint8_t w[4];
  hml_bridge_put_le32(w, 0x7f000001u);
  T_ST(hml_bridge_write(b, HML_STUB_PKTZ_BASE, w, sizeof(w)), HML_ERR_STATE,
       "pktz write refused in INIT");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &v), HML_OK, "re-read");
  T_EQ_U(v, 0u, "refused write did not reach the register");

  hml_dest_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ipv4 = 0x7f000001u;
  cfg.udp_port = 45601u;
  T_ST(hml_bridge_stub_program_dest(b, &cfg), HML_ERR_STATE, "program_dest refused in INIT");
  (void)hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &v);
  T_EQ_U(v, 0u, "refused program_dest did not reach the register");
}

static void test_start_before_config(hml_bridge_t *b) {
  T_ST(hml_bridge_set_streaming(b, true), HML_ERR_STATE, "START from INIT is refused");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "still INIT after a refused START");
  T_CHECK(!hml_bridge_is_streaming(b), "not streaming after a refused START");

  /* The same, with the direct phase setter. */
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_STREAM), HML_ERR_STATE, "set_phase(STREAM) refused");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "still INIT");

  /* Stopping when not streaming is a no-op, not an error. */
  T_ST(hml_bridge_set_streaming(b, false), HML_OK, "stop in INIT");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "stop in INIT is a no-op");
}

static void test_config(hml_bridge_t *b) {
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_CONFIG), HML_OK, "INIT -> CONFIG");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "in CONFIG");
  T_CHECK(!hml_bridge_is_streaming(b), "CONFIG is not streaming");

  /* The packetizer is writable now. */
  hml_dest_cfg cfg, got;
  memset(&cfg, 0, sizeof(cfg));
  cfg.ipv4 = 0x7f000001u;
  cfg.udp_port = 45601u;
  cfg.qp = 0x1234u;
  cfg.rkey = 0x5678u;
  T_ST(hml_bridge_stub_program_dest(b, &cfg), HML_OK, "program_dest in CONFIG");
  uint32_t v = 0;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_IP), &v), HML_OK, "read ip");
  T_EQ_U(v, 0x7f000001u, "the programmed ipv4 is in the register file");
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_OK, "get_dest in CONFIG");
  T_EQ_U(got.udp_port, 45601u, "get_dest in CONFIG");

  /* GPIO / I2C / UART still fine, and a plain poke still works. */
  uint32_t rb = 0;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x30u), 0x0f0f0f0fu, &rb), HML_OK,
       "i2c poke in CONFIG");
  T_EQ_U(rb, 0x0f0f0f0fu, "i2c readback in CONFIG");

  /* set_phase(CONFIG) from CONFIG is a no-op, not an error. */
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_CONFIG), HML_OK, "CONFIG -> CONFIG");
}

static void test_stream(hml_bridge_t *b) {
  T_ST(hml_bridge_set_streaming(b, true), HML_OK, "START from CONFIG");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "in STREAM");
  T_CHECK(hml_bridge_is_streaming(b), "streaming");

  /* Leaving STREAM with set_phase is refused: only set_streaming(false) stops. */
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_CONFIG), HML_ERR_STATE, "set_phase(CONFIG) from STREAM");
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_INIT), HML_ERR_STATE, "set_phase(INIT) from STREAM");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "still STREAM");

  /* Mid-stream pokes are allowed on purpose (HML exposures get poked live). */
  uint32_t rb = 0;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x40u), 0xa5a5a5a5u, &rb), HML_OK,
       "i2c poke while streaming");
  T_EQ_U(rb, 0xa5a5a5a5u, "i2c readback while streaming");
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_gpio_addr(HML_STUB_GPIO_REG_RESET), 0u, &rb), HML_OK,
       "gpio poke while streaming");
  uint8_t b8[3] = {1u, 2u, 3u}, bout[3] = {0u, 0u, 0u};
  T_ST(hml_bridge_write(b, HML_STUB_UART_BASE, b8, sizeof(b8)), HML_OK, "uart push while streaming");
  T_ST(hml_bridge_read(b, HML_STUB_UART_BASE, sizeof(bout), bout), HML_OK, "uart pop while streaming");
  T_CHECK(memcmp(b8, bout, sizeof(b8)) == 0, "uart round trip while streaming");

  /* The packetizer stays writable in STREAM. */
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_QP), 0x9999u, &rb), HML_OK,
       "pktz poke while streaming");
  T_EQ_U(rb, 0x9999u, "pktz readback while streaming");

  /* A redundant START is idempotent (the frozen control plane accepts a
   * repeated START too): still STREAM, still exactly one transition. */
  T_ST(hml_bridge_set_streaming(b, true), HML_OK, "START while already streaming");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "still STREAM");
  T_CHECK(hml_bridge_is_streaming(b), "still streaming");

  /* Stop: back to CONFIG, configuration kept, poke survived. */
  T_ST(hml_bridge_set_streaming(b, false), HML_OK, "STOP");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "STREAM -> CONFIG");
  T_CHECK(!hml_bridge_is_streaming(b), "not streaming after STOP");
  T_ST(hml_bridge_set_streaming(b, false), HML_OK, "stop when not streaming is harmless");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "still CONFIG");
}

static void test_reset(hml_bridge_t *b) {
  /* The mid-stream poke survived the stop. */
  uint32_t v = 0;
  T_ST(hml_bridge_read32(b, hml_bridge_stub_i2c_addr(0x40u), &v), HML_OK, "read the poke");
  T_EQ_U(v, 0xa5a5a5a5u, "the mid-stream poke survived the stop");

  /* The configuration survived too: a restart does not reprogram. */
  hml_dest_cfg got;
  T_ST(hml_bridge_stub_get_dest(b, &got), HML_OK, "get_dest after the stop");
  T_EQ_U(got.ipv4, 0x7f000001u, "dest kept across the stop");
  T_EQ_U(got.qp, 0x9999u, "the mid-stream pktz poke is still programmed");

  T_ST(hml_bridge_set_streaming(b, true), HML_OK, "restart");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "streaming again");
  T_ST(hml_bridge_set_streaming(b, false), HML_OK, "stop again");

  /* Back to INIT: the packetizer is protected again, the reset is a reset. */
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_INIT), HML_OK, "CONFIG -> INIT");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_INIT, "in INIT");
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_QP), 0u, &v), HML_ERR_STATE,
       "pktz write refused again in INIT");
  T_ST(hml_bridge_read32(b, hml_bridge_stub_pktz_addr(HML_STUB_PKTZ_REG_QP), &v), HML_OK, "read still ok");
  T_EQ_U(v, 0x9999u, "the refused write did not clear the register");

  /* And the machine can be driven round again. */
  T_ST(hml_bridge_set_phase(b, HML_BRIDGE_PHASE_CONFIG), HML_OK, "INIT -> CONFIG again");
  T_ST(hml_bridge_set_streaming(b, true), HML_OK, "START again");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_STREAM, "STREAM again");
  T_ST(hml_bridge_set_streaming(b, false), HML_OK, "STOP again");
}

static void test_phase_errors(hml_bridge_t *b) {
  T_ST(hml_bridge_set_phase(b, (hml_bridge_phase)7), HML_ERR_ARG, "out of range phase");
  T_EQ_U(hml_bridge_phase_of(b), (unsigned)HML_BRIDGE_PHASE_CONFIG, "phase unchanged");
  T_ST(hml_bridge_set_phase(NULL, HML_BRIDGE_PHASE_INIT), HML_ERR_ARG, "null bridge");
  T_EQ_U(hml_bridge_phase_of(NULL), (unsigned)HML_BRIDGE_PHASE_INIT, "null bridge reads INIT");
  T_ST(hml_bridge_set_streaming(NULL, true), HML_ERR_ARG, "null bridge streaming");
  T_CHECK(!hml_bridge_is_streaming(NULL), "null bridge is not streaming");
}

static void test_counters(hml_bridge_t *b) {
  uint32_t r0 = hml_bridge_stat_reads(b), w0 = hml_bridge_stat_writes(b), e0 = hml_bridge_stat_errors(b);
  uint32_t rb = 0;
  T_ST(hml_bridge_poke32(b, hml_bridge_stub_i2c_addr(0x50u), 1u, &rb), HML_OK, "counted poke");
  T_EQ_U(hml_bridge_stat_writes(b), w0 + 1u, "poke counted as a write");
  T_EQ_U(hml_bridge_stat_reads(b), r0 + 1u, "poke readback counted as a read");
  (void)hml_bridge_poke32(b, 0x2000u, 1u, &rb);
  T_EQ_U(hml_bridge_stat_errors(b), e0 + 1u, "a refused access counted as an error");
  T_EQ_U(hml_bridge_stat_writes(b), w0 + 1u, "a refused access is not counted as a write");
}

int main(void) {
  hml_bridge_t *b = NULL;
  T_ST(hml_bridge_stub_hml_device(&b), HML_OK, "stub device");

  test_init(b);
  test_pktz_guard_in_init(b);
  test_start_before_config(b);
  test_config(b);
  test_stream(b);
  test_reset(b);
  test_phase_errors(b);
  test_counters(b);

  hml_bridge_close(b);
  T_DONE("test_bridge_phase");
}
