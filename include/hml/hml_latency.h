#ifndef HML_LATENCY_H
#define HML_LATENCY_H

#include "hml_types.h"

/*
 * End-of-frame latency histogram (M1 exit criterion: "p99 EOF latency within
 * written budget").
 *
 * Latency is measured as (host receive clock) - (hdr->ts_us of the final
 * fragment), i.e. tx enqueue -> EOF receive, per the plan's metric list. This
 * is only meaningful when both ends share a clock domain (loopback / same host,
 * where both use hml_clock_us()) OR when the device timestamp is externally
 * disciplined. There is no PTP/NTP discipline in v1, so a negative delta means
 * the two ends do NOT share a clock: it is counted in skew_samples and NOT
 * recorded, rather than being clamped into a fake measurement.
 *
 * Percentiles use the nearest-rank definition: the smallest bucket boundary
 * v such that at least pct% of samples are <= v. If the answer lands in the
 * top bucket the result is the exact max_us instead (the tail is never an
 * interpolation).
 */

#define HML_LAT_BUCKETS 64
#define HML_LAT_DEFAULT_BUCKET_US 50u /* covers 0..3150us in 50us steps */

typedef struct hml_latency {
  uint64_t n;          /* recorded samples (skew excluded) */
  uint64_t sum_us;
  uint64_t min_us;
  uint64_t max_us;
  uint32_t bucket_us;  /* 0 = use HML_LAT_DEFAULT_BUCKET_US */
  uint32_t skew_samples;  /* negative deltas: clock domains differ */
  uint32_t overflow;      /* samples above the top bucket (kept in max_us) */
  uint32_t hist[HML_LAT_BUCKETS];
} hml_latency;

HML_API hml_status hml_latency_init(hml_latency *l, uint32_t bucket_us);
HML_API hml_status hml_latency_record(hml_latency *l, uint64_t us);
HML_API hml_status hml_latency_percentile(const hml_latency *l, unsigned pct, uint64_t *out_us);
HML_API void hml_latency_reset(hml_latency *l);

#endif /* HML_LATENCY_H */
