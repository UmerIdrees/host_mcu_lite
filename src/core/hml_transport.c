#include "hml/hml_transport.h"

#include <string.h>

#define HML_TRANSPORT_SLOTS 8

/* Fixed-capacity, registration-once transport registry (v1 is single threaded). */
static const hml_xport_ops *g_slots[HML_TRANSPORT_SLOTS];

hml_status hml_transport_register(const hml_xport_ops *ops) {
  if (!ops || !ops->name || ops->name[0] == '\0') return HML_ERR_ARG;
  if (!ops->open || !ops->send_frag || !ops->close) return HML_ERR_ARG;

  int free_slot = -1;
  for (int i = 0; i < HML_TRANSPORT_SLOTS; i++) {
    if (!g_slots[i]) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (g_slots[i] == ops || strcmp(g_slots[i]->name, ops->name) == 0) return HML_ERR_BUSY;
  }
  if (free_slot < 0) return HML_ERR_NOMEM;

  g_slots[free_slot] = ops;
  return HML_OK;
}

const hml_xport_ops *hml_transport_lookup(const char *name) {
  if (!name || name[0] == '\0') return NULL;
  for (int i = 0; i < HML_TRANSPORT_SLOTS; i++) {
    if (g_slots[i] && strcmp(g_slots[i]->name, name) == 0) return g_slots[i];
  }
  return NULL;
}
