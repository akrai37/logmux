#ifndef LOGMUX_TESTS_CHECK_H
#define LOGMUX_TESTS_CHECK_H

// Minimal assertions for the C tests. A failed check is reported and the
// test continues, so one run shows every failure; check_exit() turns the
// count into the process exit status ctest reads.

#include <inttypes.h>
#include <stdio.h>

static int check_failures;
static const char* check_context = "";

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      fprintf(stderr, "%s:%d: [%s] check failed: %s\n", __FILE__, __LINE__, check_context, #cond); \
      ++check_failures;                                                                            \
    }                                                                                              \
  } while (0)

#define CHECK_EQ_U(actual, expected)                                                               \
  do {                                                                                             \
    const uint64_t check_a_ = (uint64_t)(actual);                                                  \
    const uint64_t check_e_ = (uint64_t)(expected);                                                \
    if (check_a_ != check_e_) {                                                                    \
      fprintf(stderr, "%s:%d: [%s] %s == %" PRIu64 ", expected %" PRIu64 "\n", __FILE__, __LINE__, \
              check_context, #actual, check_a_, check_e_);                                         \
      ++check_failures;                                                                            \
    }                                                                                              \
  } while (0)

static inline int check_exit(const char* test) {
  if (check_failures != 0) {
    fprintf(stderr, "%s: %d check(s) failed\n", test, check_failures);
    return 1;
  }
  printf("%s: all checks passed\n", test);
  return 0;
}

#endif
