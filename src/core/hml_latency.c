#include "hml/hml_latency.h"

#include <string.h>

/*
 * End-of-frame latency histogram (see hml_latency.h). Bucket i covers
 * [i*b, (i+1)*b) for i in 0..HML_LAT_BUCKETS-2; the top bucket
 * (HML_LAT_BUCKETS-1) represents the tail: samples >= b*(HML_LAT_BUCKETS-1)
 * overflow into a counter and are only kept in the aggregates (n/sum/min/max),
 * never in the histogram. A percentile whose rank is only covered by the tail
 * therefore returns the exact max.
 */

hml_status hml_latency_init(hml_latency *l, uint32_t bucket_us) {
  if (!l) return HML_ERR_ARG;
  memset(l, 0, sizeof(*l));
  l->bucket_us = (bucket_us == 0) ? (uint32_t)HML_LAT_DEFAULT_BUCKET_US : bucket_us;
  l->min_us = UINT64_MAX;
  return HML_OK;
}

hml_status hml_latency_record(hml_latency *l, uint64_t us) {
  if (!l) return HML_ERR_ARG;
  uint32_t b = l->bucket_us != 0 ? l->bucket_us : (uint32_t)HML_LAT_DEFAULT_BUCKET_US;

  l->n += 1;
  l->sum_us += us;
  if (us < l->min_us) l->min_us = us;
  if (us > l->max_us) l->max_us = us;

  if (us >= (uint64_t)b * (uint64_t)(HML_LAT_BUCKETS - 1u)) {
    l->overflow += 1;
    return HML_OK;
  }
  l->hist[(size_t)(us / b)] += 1;
  return HML_OK;
}

hml_status hml_latency_percentile(const hml_latency *l, unsigned pct, uint64_t *out_us) {
  if (!l || !out_us) return HML_ERR_ARG;
  if (pct > 100) return HML_ERR_ARG;
  if (l->n == 0) return HML_ERR_AGAIN;
  if (pct == 0) {
    *out_us = l->min_us;
    return HML_OK;
  }
  if (pct == 100) {
    *out_us = l->max_us;
    return HML_OK;
  }

  uint64_t rank = (l->n * (uint64_t)pct + 99u) / 100u; /* ceil(n * pct / 100) */
  if (rank == 0) rank = 1;
  if (rank > l->n) rank = l->n;

  uint32_t b = l->bucket_us != 0 ? l->bucket_us : (uint32_t)HML_LAT_DEFAULT_BUCKET_US;
  uint64_t acc = 0;
  for (uint32_t i = 0; i < HML_LAT_BUCKETS; i++) {
    acc += (uint64_t)l->hist[i];
    if (acc >= rank) {
      if (i == (uint32_t)(HML_LAT_BUCKETS - 1u)) {
        *out_us = l->max_us; /* tail: only the exact max survives the aggregation */
      } else {
        *out_us = (uint64_t)i * (uint64_t)b; /* smallest boundary covering the rank */
      }
      return HML_OK;
    }
  }
  *out_us = l->max_us; /* rank never covered by a resolved bucket */
  return HML_OK;
}

void hml_latency_reset(hml_latency *l) {
  if (!l) return;
  uint32_t b = l->bucket_us != 0 ? l->bucket_us : (uint32_t)HML_LAT_DEFAULT_BUCKET_US;
  memset(l, 0, sizeof(*l));
  l->bucket_us = b;
  l->min_us = UINT64_MAX;
}