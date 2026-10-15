#include "hml_bridge.h"

#include <stdlib.h>
#include <string.h>

/*
 * Bridge core. See hml_bridge.h for the window map, the decode rules and the
 * phase rules. The core keeps no heap state of its own: peripherals are held in
 * a fixed table, and the one optional private slot (hml_bridge_set_priv) exists
 * so a peripheral set that does allocate (the stubs) can be freed by
 * hml_bridge_close.
 */

typedef struct hml_bridge_slot {
  char name[HML_BRIDGE_NAME_MAX];
  uint32_t base;
  uint32_t size;
  hml_bridge_read_fn rd;
  hml_bridge_write_fn wr;
  void *ctx;
} hml_bridge_slot;

struct hml_bridge {
  hml_bridge_slot slot[HML_BRIDGE_MAX_PERIPH];
  unsigned n_slot;
  hml_bridge_phase phase;
  bool streaming;
  uint32_t n_reads;
  uint32_t n_writes;
  uint32_t n_errors;
  void *priv;
  void (*priv_dtor)(void *);
};

static const char *phase_name(hml_bridge_phase p) {
  switch (p) {
    case HML_BRIDGE_PHASE_INIT: return "INIT";
    case HML_BRIDGE_PHASE_CONFIG: return "CONFIG";
    case HML_BRIDGE_PHASE_STREAM: return "STREAM";
    default: return "?";
  }
}

static bool slot_contains(const hml_bridge_slot *s, uint32_t addr) {
  return addr >= s->base && (addr - s->base) < s->size;
}

static bool window_is_pktz(uint32_t base) {
  return base >= HML_BRIDGE_PKTZ_BASE &&
         (base - HML_BRIDGE_PKTZ_BASE) < HML_BRIDGE_PKTZ_SIZE;
}

/*
 * Resolve one access. The entire span is validated before the peripheral is
 * touched, so a refusal can never leave a half applied transfer behind.
 */
static hml_status resolve(const hml_bridge_t *br, uint32_t addr, size_t n,
                          const hml_bridge_slot **out_slot, uint32_t *out_off) {
  if (out_slot) *out_slot = NULL;
  if (n == 0) return HML_ERR_ARG;

  for (unsigned i = 0; i < br->n_slot; i++) {
    const hml_bridge_slot *s = &br->slot[i];
    if (!slot_contains(s, addr)) continue;
    uint64_t off = (uint64_t)(addr - s->base);
    if (off + (uint64_t)n > (uint64_t)s->size) return HML_ERR_ARG; /* runs off the window */
    *out_slot = s;
    if (out_off) *out_off = (uint32_t)off;
    return HML_OK;
  }
  return HML_ERR_ARG; /* in no window at all */
}

static void note_error(hml_bridge_t *br) { br->n_errors++; }

hml_status hml_bridge_open(hml_bridge_t **out) {
  if (!out) return HML_ERR_ARG;
  *out = NULL;
  hml_bridge_t *br = (hml_bridge_t *)calloc(1, sizeof(*br));
  if (!br) return HML_ERR_NOMEM;
  br->phase = HML_BRIDGE_PHASE_INIT;
  *out = br;
  return HML_OK;
}

void hml_bridge_close(hml_bridge_t *b) {
  if (!b) return;
  if (b->priv_dtor) b->priv_dtor(b->priv);
  free(b);
}

hml_status hml_bridge_add_periph(hml_bridge_t *b, const hml_bridge_periph *p) {
  if (!b || !p || !p->name || p->name[0] == '\0') return HML_ERR_ARG;
  if (!p->rd || !p->wr || p->size == 0) return HML_ERR_ARG;
  if ((uint64_t)p->base + (uint64_t)p->size > 0x100000000ull) return HML_ERR_ARG;

  size_t nlen = strlen(p->name);
  if (nlen >= HML_BRIDGE_NAME_MAX) return HML_ERR_ARG;
  if (b->n_slot >= HML_BRIDGE_MAX_PERIPH) return HML_ERR_NOMEM;

  for (unsigned i = 0; i < b->n_slot; i++) {
    if (strcmp(b->slot[i].name, p->name) == 0) return HML_ERR_STATE; /* duplicate */
    if (p->base < b->slot[i].base + b->slot[i].size && b->slot[i].base < p->base + p->size)
      return HML_ERR_STATE; /* overlapping windows */
  }

  hml_bridge_slot *s = &b->slot[b->n_slot];
  memset(s, 0, sizeof(*s));
  memcpy(s->name, p->name, nlen + 1u);
  s->base = p->base;
  s->size = p->size;
  s->rd = p->rd;
  s->wr = p->wr;
  s->ctx = p->ctx;
  b->n_slot++;
  return HML_OK;
}

hml_status hml_bridge_remove_periph(hml_bridge_t *b, const char *name) {
  if (!b || !name || name[0] == '\0') return HML_ERR_ARG;
  for (unsigned i = 0; i < b->n_slot; i++) {
    if (strcmp(b->slot[i].name, name) != 0) continue;
    for (unsigned k = i + 1u; k < b->n_slot; k++) b->slot[k - 1u] = b->slot[k];
    b->n_slot--;
    memset(&b->slot[b->n_slot], 0, sizeof(b->slot[b->n_slot]));
    return HML_OK;
  }
  return HML_ERR_STATE;
}

unsigned hml_bridge_periph_count(const hml_bridge_t *b) { return b ? b->n_slot : 0u; }

hml_status hml_bridge_periph_info(const hml_bridge_t *b, const char *name, uint32_t *base,
                                   uint32_t *size) {
  if (!b || !name) return HML_ERR_ARG;
  for (unsigned i = 0; i < b->n_slot; i++) {
    if (strcmp(b->slot[i].name, name) != 0) continue;
    if (base) *base = b->slot[i].base;
    if (size) *size = b->slot[i].size;
    return HML_OK;
  }
  return HML_ERR_STATE;
}

hml_status hml_bridge_read(hml_bridge_t *b, uint32_t addr, size_t n, uint8_t *out) {
  if (!b || !out) return HML_ERR_ARG;
  const hml_bridge_slot *s = NULL;
  uint32_t off = 0;
  hml_status st = resolve(b, addr, n, &s, &off);
  if (st != HML_OK) {
    note_error(b);
    return st;
  }
  st = s->rd(s->ctx, off, n, out);
  if (st == HML_OK) b->n_reads++; else note_error(b);
  return st;
}

hml_status hml_bridge_write(hml_bridge_t *b, uint32_t addr, const uint8_t *in, size_t n) {
  if (!b || !in) return HML_ERR_ARG;
  const hml_bridge_slot *s = NULL;
  uint32_t off = 0;
  hml_status st = resolve(b, addr, n, &s, &off);
  if (st != HML_OK) {
    note_error(b);
    return st;
  }
  /* Program the destination only from CONFIG on. */
  if (window_is_pktz(s->base) && b->phase < HML_BRIDGE_PHASE_CONFIG) {
    note_error(b);
    return HML_ERR_STATE;
  }
  st = s->wr(s->ctx, off, in, n);
  if (st == HML_OK) b->n_writes++; else note_error(b);
  return st;
}

hml_status hml_bridge_poke32(hml_bridge_t *b, uint32_t addr, uint32_t val, uint32_t *readback) {
  if (!b) return HML_ERR_ARG;
  uint8_t w[4];
  hml_bridge_put_le32(w, val);
  hml_status st = hml_bridge_write(b, addr, w, sizeof(w));
  if (st != HML_OK) return st;
  if (!readback) return HML_OK;
  uint8_t r[4];
  st = hml_bridge_read(b, addr, sizeof(r), r);
  if (st != HML_OK) return st;
  *readback = hml_bridge_get_le32(r);
  return HML_OK;
}

hml_status hml_bridge_read32(hml_bridge_t *b, uint32_t addr, uint32_t *out) {
  if (!b || !out) return HML_ERR_ARG;
  uint8_t r[4];
  hml_status st = hml_bridge_read(b, addr, sizeof(r), r);
  if (st != HML_OK) return st;
  *out = hml_bridge_get_le32(r);
  return HML_OK;
}

hml_bridge_phase hml_bridge_phase_of(const hml_bridge_t *b) {
  return b ? b->phase : HML_BRIDGE_PHASE_INIT;
}

hml_status hml_bridge_set_phase(hml_bridge_t *b, hml_bridge_phase phase) {
  if (!b) return HML_ERR_ARG;
  if (phase != HML_BRIDGE_PHASE_INIT && phase != HML_BRIDGE_PHASE_CONFIG &&
      phase != HML_BRIDGE_PHASE_STREAM)
    return HML_ERR_ARG;
  if (phase == b->phase) return HML_OK; /* idempotent */

  /* STREAM is entered by hml_bridge_set_streaming(true) only. */
  if (phase == HML_BRIDGE_PHASE_STREAM) return HML_ERR_STATE;
  /* Leaving STREAM is a stop, not a reconfigure. */
  if (b->phase == HML_BRIDGE_PHASE_STREAM) return HML_ERR_STATE;

  b->phase = phase;
  return HML_OK;
}

bool hml_bridge_is_streaming(const hml_bridge_t *b) { return b ? b->streaming : false; }

hml_status hml_bridge_set_streaming(hml_bridge_t *b, bool streaming) {
  if (!b) return HML_ERR_ARG;
  if (!streaming) {
    b->streaming = false;
    if (b->phase == HML_BRIDGE_PHASE_STREAM) b->phase = HML_BRIDGE_PHASE_CONFIG;
    return HML_OK;
  }
  if (b->phase < HML_BRIDGE_PHASE_CONFIG) return HML_ERR_STATE; /* never stream unconfigured */
  b->streaming = true;
  b->phase = HML_BRIDGE_PHASE_STREAM;
  return HML_OK;
}

uint32_t hml_bridge_stat_reads(const hml_bridge_t *b) { return b ? b->n_reads : 0u; }
uint32_t hml_bridge_stat_writes(const hml_bridge_t *b) { return b ? b->n_writes : 0u; }
uint32_t hml_bridge_stat_errors(const hml_bridge_t *b) { return b ? b->n_errors : 0u; }

hml_status hml_bridge_set_priv(hml_bridge_t *b, void *priv, void (*dtor)(void *)) {
  if (!b) return HML_ERR_ARG;
  if (b->priv && b->priv != priv) return HML_ERR_STATE; /* already taken */
  b->priv = priv;
  b->priv_dtor = dtor;
  return HML_OK;
}

void *hml_bridge_priv(const hml_bridge_t *b) { return b ? b->priv : NULL; }

/* Kept out of the header: the phase name is for logging only. */
const char *hml_bridge_phase_str(hml_bridge_phase p) { return phase_name(p); }
