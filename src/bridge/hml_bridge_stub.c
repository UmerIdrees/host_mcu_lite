#include "hml_bridge_stub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Stub peripherals. Every block is a plain byte array inside one shared device
 * state, so the window callbacks are memcpy plus the little bit of block
 * behaviour a real device would have (reset flag, FIFO, NAK fault). The shared
 * state is attached to the bridge private slot and freed by hml_bridge_close.
 */

#define STUB_MAGIC 0x53545242u /* 'STRB' */

typedef struct hml_bridge_stub_dev {
  uint32_t magic;
  struct {
    uint8_t bytes[HML_STUB_GPIO_SIZE];
    bool reset_asserted;
  } gpio;
  struct {
    uint8_t bytes[HML_STUB_I2C_SIZE];
    int nak_after;     /* < 0 disarmed */
    uint32_t n_writes; /* successful writes since the fault was armed */
  } i2c;
  struct {
    uint8_t fifo[HML_STUB_UART_FIFO];
    unsigned head;
    unsigned tail;
    unsigned count;
  } uart;
  struct {
    uint8_t bytes[HML_STUB_PKTZ_SIZE];
    hml_dest_cfg shadow;
    bool have_shadow;
  } pktz;
} hml_bridge_stub_dev_t;

static hml_bridge_stub_dev_t *stub_of(hml_bridge_t *b) {
  if (!b) return NULL;
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)hml_bridge_priv(b);
  if (!d || d->magic != STUB_MAGIC) return NULL;
  return d;
}

static void stub_free(void *priv) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)priv;
  if (!d) return;
  d->magic = 0u;
  free(d);
}

/* ---- gpio: 0x00 reset, 0x04 read-only reset flag ------------------------- */

static hml_status gpio_rd(void *ctx, uint32_t addr, size_t n, uint8_t *out) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr > HML_STUB_GPIO_SIZE || n > (size_t)HML_STUB_GPIO_SIZE - addr) return HML_ERR_ARG;
  if (d->gpio.reset_asserted) hml_bridge_put_le32(d->gpio.bytes + 4u * HML_STUB_GPIO_REG_RESET_FLAG, 1u);
  memcpy(out, d->gpio.bytes + addr, n);
  return HML_OK;
}

static hml_status gpio_wr(void *ctx, uint32_t addr, const uint8_t *in, size_t n) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr > HML_STUB_GPIO_SIZE || n > (size_t)HML_STUB_GPIO_SIZE - addr) return HML_ERR_ARG;
  uint8_t flag[4];
  memcpy(flag, d->gpio.bytes + 4u * HML_STUB_GPIO_REG_RESET_FLAG, sizeof(flag));

  memcpy(d->gpio.bytes + addr, in, n);

  if (hml_bridge_get_le32(d->gpio.bytes) != 0u) d->gpio.reset_asserted = true;
  /* RESET_FLAG is read-only: writes to it are accepted and discarded. */
  if (d->gpio.reset_asserted)
    hml_bridge_put_le32(d->gpio.bytes + 4u * HML_STUB_GPIO_REG_RESET_FLAG, 1u);
  else
    memcpy(d->gpio.bytes + 4u * HML_STUB_GPIO_REG_RESET_FLAG, flag, sizeof(flag));
  return HML_OK;
}

/* ---- i2c: 256 x u32 register file, NAK fault ----------------------------- */

static hml_status i2c_rd(void *ctx, uint32_t addr, size_t n, uint8_t *out) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr > HML_STUB_I2C_SIZE || n > (size_t)HML_STUB_I2C_SIZE - addr) return HML_ERR_ARG;
  memcpy(out, d->i2c.bytes + addr, n);
  return HML_OK;
}

static hml_status i2c_wr(void *ctx, uint32_t addr, const uint8_t *in, size_t n) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr > HML_STUB_I2C_SIZE || n > (size_t)HML_STUB_I2C_SIZE - addr) return HML_ERR_ARG;
  if (d->i2c.nak_after >= 0 && (int)d->i2c.n_writes >= d->i2c.nak_after) return HML_ERR_IO;
  memcpy(d->i2c.bytes + addr, in, n);
  d->i2c.n_writes++;
  return HML_OK;
}

/* ---- uart: 256-byte circular FIFO --------------------------------------- */

static hml_status uart_rd(void *ctx, uint32_t addr, size_t n, uint8_t *out) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr >= HML_STUB_UART_SIZE || n == 0 || n > (size_t)HML_STUB_UART_SIZE - addr)
    return HML_ERR_ARG;
  if (d->uart.count < n) return HML_ERR_AGAIN; /* all-or-nothing, nothing popped */
  for (size_t i = 0; i < n; i++) {
    out[i] = d->uart.fifo[d->uart.tail];
    d->uart.tail = (d->uart.tail + 1u) % HML_STUB_UART_FIFO;
  }
  d->uart.count -= (unsigned)n;
  return HML_OK;
}

static hml_status uart_wr(void *ctx, uint32_t addr, const uint8_t *in, size_t n) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr >= HML_STUB_UART_SIZE || n == 0 || n > (size_t)HML_STUB_UART_SIZE - addr)
    return HML_ERR_ARG;
  if (d->uart.count + n > HML_STUB_UART_FIFO) return HML_ERR_BUSY; /* full, back-pressure */
  for (size_t i = 0; i < n; i++) {
    d->uart.fifo[d->uart.head] = in[i];
    d->uart.head = (d->uart.head + 1u) % HML_STUB_UART_FIFO;
  }
  d->uart.count += (unsigned)n;
  return HML_OK;
}

/* ---- packetizer: 4 x u32 dest words ------------------------------------- */

static hml_status pktz_rd(void *ctx, uint32_t addr, size_t n, uint8_t *out) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr > HML_STUB_PKTZ_SIZE || n > (size_t)HML_STUB_PKTZ_SIZE - addr) return HML_ERR_ARG;
  memcpy(out, d->pktz.bytes + addr, n);
  return HML_OK;
}

static hml_status pktz_wr(void *ctx, uint32_t addr, const uint8_t *in, size_t n) {
  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)ctx;
  if (addr > HML_STUB_PKTZ_SIZE || n > (size_t)HML_STUB_PKTZ_SIZE - addr) return HML_ERR_ARG;
  memcpy(d->pktz.bytes + addr, in, n);
  return HML_OK;
}

/* ---- device -------------------------------------------------------------- */

hml_status hml_bridge_stub_hml_device(hml_bridge_t **out) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;

  hml_bridge_t *b = NULL;
  hml_status st = hml_bridge_open(&b);
  if (st != HML_OK) return st;

  hml_bridge_stub_dev_t *d = (hml_bridge_stub_dev_t *)calloc(1, sizeof(*d));
  if (!d) {
    hml_bridge_close(b);
    return HML_ERR_NOMEM;
  }
  d->magic = STUB_MAGIC;
  d->i2c.nak_after = -1;
  st = hml_bridge_set_priv(b, d, stub_free);
  if (st != HML_OK) {
    stub_free(d);
    hml_bridge_close(b);
    return st;
  }

  hml_bridge_periph p[4];
  p[0].name = "gpio";       p[0].base = HML_STUB_GPIO_BASE;  p[0].size = HML_STUB_GPIO_SIZE;
  p[0].rd = gpio_rd;       p[0].wr = gpio_wr;               p[0].ctx = d;
  p[1].name = "i2c";        p[1].base = HML_STUB_I2C_BASE;   p[1].size = HML_STUB_I2C_SIZE;
  p[1].rd = i2c_rd;        p[1].wr = i2c_wr;                p[1].ctx = d;
  p[2].name = "uart";       p[2].base = HML_STUB_UART_BASE;  p[2].size = HML_STUB_UART_SIZE;
  p[2].rd = uart_rd;       p[2].wr = uart_wr;               p[2].ctx = d;
  p[3].name = "packetizer"; p[3].base = HML_STUB_PKTZ_BASE;  p[3].size = HML_STUB_PKTZ_SIZE;
  p[3].rd = pktz_rd;       p[3].wr = pktz_wr;               p[3].ctx = d;

  for (unsigned i = 0; i < 4u; i++) {
    st = hml_bridge_add_periph(b, &p[i]);
    if (st != HML_OK) {
      hml_bridge_close(b);
      return st;
    }
  }
  *out = b;
  return HML_OK;
}

hml_status hml_bridge_stub_set_nak_after(hml_bridge_t *b, int n_writes) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d) return HML_ERR_STATE;
  d->i2c.nak_after = n_writes < 0 ? -1 : n_writes;
  d->i2c.n_writes = 0u;
  return HML_OK;
}

uint32_t hml_bridge_stub_i2c_writes(const hml_bridge_t *b) {
  hml_bridge_stub_dev_t *d = stub_of((hml_bridge_t *)b);
  return d ? d->i2c.n_writes : 0u;
}

hml_status hml_bridge_stub_i2c_get(hml_bridge_t *b, uint32_t reg, uint32_t *out) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d) return HML_ERR_STATE;
  if (!out || reg >= HML_STUB_I2C_REGS) return HML_ERR_ARG;
  *out = hml_bridge_get_le32(d->i2c.bytes + reg * 4u);
  return HML_OK;
}

hml_status hml_bridge_stub_i2c_set(hml_bridge_t *b, uint32_t reg, uint32_t val) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d) return HML_ERR_STATE;
  if (reg >= HML_STUB_I2C_REGS) return HML_ERR_ARG;
  hml_bridge_put_le32(d->i2c.bytes + reg * 4u, val);
  return HML_OK;
}

hml_status hml_bridge_stub_gpio_get(hml_bridge_t *b, uint32_t reg, uint32_t *out) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d) return HML_ERR_STATE;
  if (!out || reg >= HML_STUB_GPIO_REGS) return HML_ERR_ARG;
  *out = hml_bridge_get_le32(d->gpio.bytes + reg * 4u);
  return HML_OK;
}

bool hml_bridge_stub_gpio_reset_asserted(hml_bridge_t *b) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  return d ? d->gpio.reset_asserted : false;
}

uint32_t hml_bridge_stub_uart_count(hml_bridge_t *b) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  return d ? (uint32_t)d->uart.count : 0u;
}

hml_status hml_bridge_stub_program_dest(hml_bridge_t *b, const hml_dest_cfg *cfg) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d || !cfg) return d ? HML_ERR_ARG : HML_ERR_STATE;

  uint8_t w[4u * HML_STUB_PKTZ_REGS];
  hml_bridge_put_le32(w + 0u * 4u, cfg->ipv4);
  hml_bridge_put_le32(w + 1u * 4u, (uint32_t)(cfg->udp_port & 0xffffu));
  hml_bridge_put_le32(w + 2u * 4u, cfg->qp);
  hml_bridge_put_le32(w + 3u * 4u, cfg->rkey);

  hml_status st = hml_bridge_write(b, HML_STUB_PKTZ_BASE, w, sizeof(w));
  if (st != HML_OK) return st;

  d->pktz.shadow = *cfg;
  d->pktz.have_shadow = true;
  return HML_OK;
}

hml_status hml_bridge_stub_get_dest(hml_bridge_t *b, hml_dest_cfg *out) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d) return HML_ERR_STATE;
  if (!out) return HML_ERR_ARG;
  if (!d->pktz.have_shadow) return HML_ERR_STATE;

  uint8_t w[4u * HML_STUB_PKTZ_REGS];
  hml_status st = hml_bridge_read(b, HML_STUB_PKTZ_BASE, sizeof(w), w);
  if (st != HML_OK) return st;

  /* The four words come from the register file, the rest from the shadow. */
  *out = d->pktz.shadow;
  out->ipv4 = hml_bridge_get_le32(w + 0u * 4u);
  out->udp_port = (uint16_t)hml_bridge_get_le32(w + 1u * 4u);
  out->qp = hml_bridge_get_le32(w + 2u * 4u);
  out->rkey = hml_bridge_get_le32(w + 3u * 4u);
  return HML_OK;
}

hml_status hml_bridge_stub_dump(hml_bridge_t *b, char *out, size_t cap) {
  hml_bridge_stub_dev_t *d = stub_of(b);
  if (!d) return HML_ERR_STATE;
  if (!out || cap == 0) return HML_ERR_ARG;
  (void)snprintf(out, cap,
                 "gpio reset=0x%08x flag=0x%08x | i2c[0x10]=0x%08x i2c[0x11]=0x%08x | uart=%u | "
                 "pktz ip=0x%08x port=%u qp=0x%08x rkey=0x%08x",
                 (unsigned)hml_bridge_get_le32(d->gpio.bytes),
                 (unsigned)(d->gpio.reset_asserted ? 1u : 0u),
                 (unsigned)hml_bridge_get_le32(d->i2c.bytes + 0x10u * 4u),
                 (unsigned)hml_bridge_get_le32(d->i2c.bytes + 0x11u * 4u), d->uart.count,
                 (unsigned)hml_bridge_get_le32(d->pktz.bytes + 0u),
                 (unsigned)((uint16_t)hml_bridge_get_le32(d->pktz.bytes + 4u)),
                 (unsigned)hml_bridge_get_le32(d->pktz.bytes + 8u),
                 (unsigned)hml_bridge_get_le32(d->pktz.bytes + 12u));
  return HML_OK;
}
