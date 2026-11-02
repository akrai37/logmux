#ifndef LOGMUX_TOOLS_HISTOGRAM_H
#define LOGMUX_TOOLS_HISTOGRAM_H

#include <stdint.h>
#include <string.h>

// Log-linear latency histogram in the style of HdrHistogram: values below
// 16 ns are exact, larger ones fall into 16 sub-buckets per power of two, so
// a bucket's width is at most 1/16 (6.25%) of its value. Recording is a few
// instructions with no allocation; per-thread histograms merge by addition.

#define SL_HIST_SUB_BITS 4
#define SL_HIST_SUB (1u << SL_HIST_SUB_BITS)
#define SL_HIST_BUCKETS 1024

typedef struct {
  uint64_t counts[SL_HIST_BUCKETS];
  uint64_t total;
  uint64_t max;
} sl_histogram;

static inline unsigned sl_hist_index(uint64_t ns) {
  if (ns < SL_HIST_SUB) {
    return (unsigned)ns;
  }
  const unsigned exp = 63u - (unsigned)__builtin_clzll(ns);  // ns in [2^exp, 2^(exp+1))
  const unsigned sub = (unsigned)(ns >> (exp - SL_HIST_SUB_BITS)) & (SL_HIST_SUB - 1);
  return (exp - SL_HIST_SUB_BITS + 1) * SL_HIST_SUB + sub;
}

// Smallest value that maps to bucket `index`.
static inline uint64_t sl_hist_lower(unsigned index) {
  if (index < SL_HIST_SUB) {
    return index;
  }
  const unsigned exp = index / SL_HIST_SUB + SL_HIST_SUB_BITS - 1;
  return (uint64_t)(SL_HIST_SUB + index % SL_HIST_SUB) << (exp - SL_HIST_SUB_BITS);
}

static inline void sl_hist_record(sl_histogram* h, uint64_t ns) {
  ++h->counts[sl_hist_index(ns)];
  ++h->total;
  if (ns > h->max) {
    h->max = ns;
  }
}

static inline void sl_hist_merge(sl_histogram* into, const sl_histogram* from) {
  for (unsigned i = 0; i < SL_HIST_BUCKETS; ++i) {
    into->counts[i] += from->counts[i];
  }
  into->total += from->total;
  if (from->max > into->max) {
    into->max = from->max;
  }
}

// Value at quantile q in [0, 1], reported as the lower edge of its bucket
// (within 6.25% of the true value). 0 for an empty histogram.
static inline uint64_t sl_hist_quantile(const sl_histogram* h, double q) {
  if (h->total == 0) {
    return 0;
  }
  uint64_t rank = (uint64_t)(q * (double)h->total);
  if (rank >= h->total) {
    rank = h->total - 1;
  }
  uint64_t seen = 0;
  for (unsigned i = 0; i < SL_HIST_BUCKETS; ++i) {
    seen += h->counts[i];
    if (seen > rank) {
      return sl_hist_lower(i);
    }
  }
  return h->max;
}

static inline void sl_hist_clear(sl_histogram* h) {
  memset(h, 0, sizeof *h);
}

#endif
