/* M1: hml_latency histogram known-answer, exactness, overflow and distribution
 * checks. The semantics under test are those of hml_latency.h: nearest-rank
 * percentiles over 64 buckets, tail samples that overflow the histogram but not
 * the aggregates, and the top-bucket rule (ranks only covered by the tail
 * return the exact max_us). */
#include "hml/hml_latency.h"
#include "hml_test_util.h"

static void check_pct(const hml_latency *l, unsigned pct, uint64_t want) {
  uint64_t got = 0;
  CHECK_STATUS(hml_latency_percentile(l, pct, &got), HML_OK);
  CHECK_EQ_U32(got, want);
}

static void feed(hml_latency *l, uint64_t first, uint64_t last) {
  for (uint64_t v = first; v <= last; v++) CHECK_STATUS(hml_latency_record(l, v), HML_OK);
}

int main(void) {
  /* ---- init: default bucket width, UINT64_MAX min sentinel, reset keeps width */
  hml_latency l;
  CHECK_STATUS(hml_latency_init(&l, 10), HML_OK);
  CHECK_EQ_U32(l.bucket_us, 10u);
  CHECK(l.min_us == UINT64_MAX);
  CHECK_STATUS(hml_latency_init(&l, 0), HML_OK); /* 0 selects the default */
  CHECK_EQ_U32(l.bucket_us, (uint32_t)HML_LAT_DEFAULT_BUCKET_US);

  /* empty histogram: AGAIN */
  CHECK_STATUS(hml_latency_percentile(&l, 50, &(uint64_t){0}), HML_ERR_AGAIN);

  /* bad args: NULL pointer, pct > 100 */
  uint64_t tmp = 0;
  CHECK_STATUS(hml_latency_percentile(NULL, 50, &tmp), HML_ERR_ARG);
  CHECK_STATUS(hml_latency_percentile(&l, 50, NULL), HML_ERR_ARG);
  CHECK_STATUS(hml_latency_record(NULL, 5u), HML_ERR_ARG);
  CHECK_STATUS(hml_latency_init(NULL, 10), HML_ERR_ARG);

  /* reset restores the empty state while preserving the configured width */
  CHECK_STATUS(hml_latency_record(&l, 5u), HML_OK);
  hml_latency_reset(&l);
  CHECK_EQ_U32(l.bucket_us, (uint32_t)HML_LAT_DEFAULT_BUCKET_US);
  CHECK_EQ_U32(l.n, 0u);
  CHECK_STATUS(hml_latency_percentile(&l, 50, &tmp), HML_ERR_AGAIN);

  /* ---- known-answer: one sample on every resolvable bucket boundary ----
   * us = 10*i for i in 0..62 -> hist[0..62], one sample per bucket; a sample at
   * 630 or above is the overflow tail, so the 64th bucket is reached by the
   * tail, not by a resolved histogram entry. */
  hml_latency l10;
  CHECK_STATUS(hml_latency_init(&l10, 10), HML_OK);
  for (uint64_t i = 0; i < 63u; i++) CHECK_STATUS(hml_latency_record(&l10, i * 10u), HML_OK);
  CHECK_EQ_U32(l10.n, 63u);
  CHECK_EQ_U32(l10.sum_us, 10u * 63u * 62u / 2u);
  CHECK_EQ_U32(l10.min_us, 0u);
  CHECK_EQ_U32(l10.max_us, 620u);
  CHECK_EQ_U32(l10.overflow, 0u);
  check_pct(&l10, 0, 0u);    /* pct 0 -> min */
  check_pct(&l10, 100, 620u); /* pct 100 -> max */
  check_pct(&l10, 50, 310u); /* nearest rank: 32nd sample = 310 */
  CHECK_STATUS(hml_latency_percentile(&l10, 101, &tmp), HML_ERR_ARG);

  /* ---- exactness: samples 1..1000 us at bucket 10 ---- */
  hml_latency ex;
  CHECK_STATUS(hml_latency_init(&ex, 10), HML_OK);
  feed(&ex, 1u, 1000u);
  CHECK_EQ_U32(ex.n, 1000u);
  /* analytic nearest-rank answer for uniform spacing 1..1000 is the sample
   * value itself: p50 -> 500, p99 -> 990. Both must be within one bucket. */
  {
    uint64_t p50 = 0, p99 = 0;
    CHECK_STATUS(hml_latency_percentile(&ex, 50, &p50), HML_OK);
    CHECK_STATUS(hml_latency_percentile(&ex, 99, &p99), HML_OK);
    CHECK(p50 >= 490u && p50 <= 510u);
    CHECK(p99 >= 980u && p99 <= 1000u);
    /* p99's rank is covered only by the tail (values >= 630 overflow the
     * histogram): the top-bucket rule returns the exact max, not a boundary. */
    CHECK_EQ_U32(p99, ex.max_us);
    CHECK_EQ_U32(p99, 1000u);
    CHECK_EQ_U32(ex.overflow, 1000u - 630u + 1u);
  }

  /* ---- overflow: 1,000,000 us at bucket 50 ---- */
  hml_latency ov;
  CHECK_STATUS(hml_latency_init(&ov, 50), HML_OK);
  CHECK_STATUS(hml_latency_record(&ov, 1000000u), HML_OK);
  CHECK_EQ_U32(ov.n, 1u);
  CHECK_EQ_U32(ov.sum_us, 1000000u);
  CHECK_EQ_U32(ov.min_us, 1000000u);
  CHECK_EQ_U32(ov.max_us, 1000000u);
  CHECK_EQ_U32(ov.overflow, 1u);
  check_pct(&ov, 50, 1000000u); /* aggregates intact -> a sane tail percentile */

  /* ---- distribution: 1000 samples evenly spread over 100..199 us ---- */
  hml_latency d;
  CHECK_STATUS(hml_latency_init(&d, 10), HML_OK);
  for (unsigned k = 0; k < 10u; k++) feed(&d, 100u, 199u);
  CHECK_EQ_U32(d.n, 1000u);
  {
    uint64_t p50 = 0, p99 = 0;
    CHECK_STATUS(hml_latency_percentile(&d, 50, &p50), HML_OK);
    CHECK_STATUS(hml_latency_percentile(&d, 99, &p99), HML_OK);
    CHECK(p50 >= 140u && p50 <= 160u); /* analytic p50 = 150, within 2 buckets */
    CHECK(p99 >= 188u && p99 <= 208u); /* analytic p99 = 198, within 2 buckets */
  }

  TEST_DONE();
}