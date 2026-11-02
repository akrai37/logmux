#ifndef LOGMUX_TESTS_VERIFY_H
#define LOGMUX_TESTS_VERIFY_H

// Exact verification of a log against what logmux-child wrote. For each
// child the expected text of each stream is regenerated from the seed, and
// the log's records for that child and stream, joined back together, must
// equal it byte for byte. That rules out lost, duplicated, reordered, torn
// or interleaved lines in one comparison. Unterminated final lines ('~')
// must appear exactly where the mode produces one.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "payload.h"

typedef struct {
  const char* name;
  unsigned id;
  const char* mode;
  unsigned long long lines;
  unsigned long long seed;
  size_t max_len;
} child_expect;

typedef struct {
  char* text;
  size_t len;
  size_t cap;
} text_buf;

static inline void tb_append(text_buf* b, const char* s, size_t n) {
  while (b->len + n + 1 > b->cap) {
    b->cap = b->cap == 0 ? 1024 : b->cap * 2;
    b->text = realloc(b->text, b->cap);
  }
  memcpy(b->text + b->len, s, n);
  b->len += n;
  b->text[b->len] = '\0';
}

// Appends lines [from, to) of the child, each followed by '\n', optionally
// only those that payload.h assigns to `stream`.
static inline void tb_lines(text_buf* b, const child_expect* e, unsigned long long from,
                            unsigned long long to, int only_stream) {
  char* line = malloc(lm_line_capacity(e->max_len) + 1);
  for (unsigned long long s = from; s < to; ++s) {
    if (only_stream >= 0 && lm_line_stream(e->seed, e->id, s) != only_stream) {
      continue;
    }
    const size_t len = lm_make_line(line, e->seed, e->id, s, e->max_len);
    line[len] = '\n';
    tb_append(b, line, len + 1);
  }
  free(line);
}

// Expected joined text of a stream (as lines_of returns it) and the number
// of '~' records it must contain.
static inline char* expected_stream(const child_expect* e, int stream, int* tildes) {
  text_buf b = {0};
  tb_append(&b, "", 0);
  *tildes = 0;
  const char* m = e->mode;
  const unsigned long long n = e->lines;
  if (strcmp(m, "normal") == 0 || strcmp(m, "flood") == 0 || strcmp(m, "trickle") == 0 ||
      strcmp(m, "hang") == 0) {
    if (stream == 0) {
      tb_lines(&b, e, 0, n, -1);
    }
  } else if (strcmp(m, "both") == 0) {
    tb_lines(&b, e, 0, n, stream);
  } else if (strcmp(m, "crash") == 0) {
    if (stream == 0) {
      tb_lines(&b, e, 0, n, -1);
      char* line = malloc(lm_line_capacity(e->max_len) + 1);
      const size_t len = lm_make_line(line, e->seed, e->id, n, e->max_len);
      if (len / 2 > 0) {
        tb_append(&b, line, len / 2);
        tb_append(&b, "\n", 1);
        *tildes = 1;
      }
      free(line);
    }
  } else if (strcmp(m, "no-newline") == 0) {
    if (stream == 0) {
      tb_lines(&b, e, 0, n, -1);
      *tildes = n > 0 ? 1 : 0;
    }
  } else if (strcmp(m, "early-close") == 0) {
    tb_lines(&b, e, 0, stream == 0 ? n / 2 : n, -1);
  } else if (strcmp(m, "stall-stderr") == 0) {
    if (stream == 0) {
      tb_lines(&b, e, 0, n, -1);
    } else {
      char line[64];
      for (unsigned long long s = 0; s < 4096; ++s) {
        const int len = snprintf(line, sizeof line, "err filler %llu %031d\n", s, 0);
        tb_append(&b, line, (size_t)len);
      }
    }
  }
  return b.text;
}

static inline int count_marks(const log_view* log, const char* name, const char* stream,
                              char mark) {
  int n = 0;
  for (size_t i = 0; i < log->count; ++i) {
    const record* r = &log->records[i];
    if (r->mark == mark && strcmp(r->name, name) == 0 && strcmp(r->stream, stream) == 0) {
      ++n;
    }
  }
  return n;
}

// True if the log holds exactly what the child wrote. On failure, `why`
// says where the first difference is.
static inline bool verify_child(const log_view* log, const child_expect* e, char* why,
                                size_t why_size) {
  static const char* const streams[2] = {"out", "err"};
  for (int k = 0; k < 2; ++k) {
    int tildes = 0;
    char* want = expected_stream(e, k, &tildes);
    char* got = lines_of(log, e->name, streams[k]);
    bool ok = strcmp(want, got) == 0;
    if (!ok) {
      size_t at = 0;
      while (want[at] != '\0' && want[at] == got[at]) {
        ++at;
      }
      size_t line = 1;
      for (size_t i = 0; i < at; ++i) {
        line += want[i] == '\n' ? 1u : 0u;
      }
      snprintf(why, why_size,
               "%s %s: differs at byte %zu (line %zu) of %zu expected, %zu collected", e->name,
               streams[k], at, line, strlen(want), strlen(got));
    } else if (count_marks(log, e->name, streams[k], '~') != tildes) {
      ok = false;
      snprintf(why, why_size, "%s %s: %d unterminated-line records, expected %d", e->name,
               streams[k], count_marks(log, e->name, streams[k], '~'), tildes);
    }
    free(want);
    free(got);
    if (!ok) {
      return false;
    }
  }
  return true;
}

#endif
