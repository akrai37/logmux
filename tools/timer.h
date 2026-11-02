#ifndef LOGMUX_TOOLS_TIMER_H
#define LOGMUX_TOOLS_TIMER_H

#include <stdint.h>

// Monotonic nanosecond clock for per-operation latency.
//
// On macOS, POSIX CLOCK_MONOTONIC only advances in whole microseconds, which
// is longer than an uncontended booking takes. mach_absolute_time is the
// finest clock user code can read there: 24 MHz on Apple silicon, so one
// tick is about 41.7 ns. On Linux, CLOCK_MONOTONIC has nanosecond resolution.
// sl_timer_resolution_ns reports the tick so results can say which
// latencies are below what the clock can resolve.

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>

static mach_timebase_info_data_t sl_timebase_value;
static pthread_once_t sl_timebase_once = PTHREAD_ONCE_INIT;

static inline void sl_timebase_init(void) {
  mach_timebase_info(&sl_timebase_value);
}

// Tick-to-nanosecond ratio, read once per process without a data race.
static inline mach_timebase_info_data_t sl_timebase(void) {
  pthread_once(&sl_timebase_once, sl_timebase_init);
  return sl_timebase_value;
}

static inline uint64_t sl_now_ns(void) {
  const mach_timebase_info_data_t tb = sl_timebase();
  return mach_absolute_time() * tb.numer / tb.denom;
}

static inline double sl_timer_resolution_ns(void) {
  const mach_timebase_info_data_t tb = sl_timebase();
  return (double)tb.numer / (double)tb.denom;
}
#else
#include <time.h>

static inline uint64_t sl_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static inline double sl_timer_resolution_ns(void) {
  struct timespec ts;
  clock_getres(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}
#endif

#endif
