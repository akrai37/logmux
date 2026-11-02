#ifndef LOGMUX_TESTS_HARNESS_H
#define LOGMUX_TESTS_HARNESS_H

// Runs lm_run with the log captured in a temporary file, then parses the log
// back into records so tests can check exactly what was written.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logmux/collector.h"

typedef struct {
  double seconds;  // timestamp: seconds since logmux started
  char name[64];
  char stream[4];  // "out", "err" or "sys"
  char mark;       // ':', '+' or '~'
  char* text;
} record;

typedef struct {
  record* records;
  size_t count;
  char* raw;
  size_t raw_len;
  bool well_formed;  // every line parsed as a record
} log_view;

static inline void log_free(log_view* log) {
  for (size_t i = 0; i < log->count; ++i) {
    free(log->records[i].text);
  }
  free(log->records);
  free(log->raw);
  memset(log, 0, sizeof *log);
}

// Parses "<seconds> <name> <stream><mark> <text>" lines.
static inline log_view log_parse(const char* raw, size_t len) {
  log_view log = {.well_formed = true};
  log.raw = malloc(len + 1);
  memcpy(log.raw, raw, len);
  log.raw[len] = '\0';
  log.raw_len = len;
  size_t cap = 16;
  log.records = malloc(cap * sizeof *log.records);
  const char* p = raw;
  const char* end = raw + len;
  while (p < end) {
    const char* nl = memchr(p, '\n', (size_t)(end - p));
    if (nl == NULL) {
      log.well_formed = false;  // the log itself must always end with a newline
      break;
    }
    record r;
    memset(&r, 0, sizeof r);
    const char* sp1 = memchr(p, ' ', (size_t)(nl - p));
    const char* sp2 = sp1 != NULL ? memchr(sp1 + 1, ' ', (size_t)(nl - sp1 - 1)) : NULL;
    if (sp2 == NULL || nl - sp2 < 5 || sp2[5] != ' ' || (size_t)(sp2 - sp1 - 1) >= sizeof r.name) {
      log.well_formed = false;
      p = nl + 1;
      continue;
    }
    r.seconds = strtod(p, NULL);
    memcpy(r.name, sp1 + 1, (size_t)(sp2 - sp1 - 1));
    memcpy(r.stream, sp2 + 1, 3);
    r.mark = sp2[4];
    const char* text = sp2 + 6;
    r.text = malloc((size_t)(nl - text) + 1);
    memcpy(r.text, text, (size_t)(nl - text));
    r.text[nl - text] = '\0';
    if (log.count == cap) {
      cap *= 2;
      log.records = realloc(log.records, cap * sizeof *log.records);
    }
    log.records[log.count++] = r;
    p = nl + 1;
  }
  return log;
}

// Runs the children with the log written to a temporary file and returns
// the parsed log. `config` may leave out_fd unset.
static inline log_view run_and_capture(lm_config config, const lm_child_spec* specs, size_t n,
                                       lm_child_result* results, lm_stats* stats, int* rc) {
  FILE* tmp = tmpfile();
  config.out_fd = fileno(tmp);
  *rc = lm_run(&config, specs, n, results, stats);
  // lm_run wrote through the descriptor, so ask the file itself for its size.
  fseek(tmp, 0, SEEK_END);
  const long size = ftell(tmp) > 0 ? ftell(tmp) : 0;
  char* raw = malloc((size_t)size + 1);
  rewind(tmp);
  const size_t got = fread(raw, 1, (size_t)size, tmp);
  fclose(tmp);
  log_view log = log_parse(raw, got);
  free(raw);
  return log;
}

// The records of one child and stream, in log order, joined back together:
// pieces ('+') are concatenated with what follows, and each ':' or '~'
// record ends a line. Lines are separated by '\n' in the returned string;
// `fragments_seen` counts '~' records.
static inline char* lines_of(const log_view* log, const char* name, const char* stream) {
  size_t cap = 256;
  size_t len = 0;
  char* out = malloc(cap);
  out[0] = '\0';
  for (size_t i = 0; i < log->count; ++i) {
    const record* r = &log->records[i];
    if (strcmp(r->name, name) != 0 || strcmp(r->stream, stream) != 0) {
      continue;
    }
    const size_t t = strlen(r->text);
    while (len + t + 2 > cap) {
      cap *= 2;
      out = realloc(out, cap);
    }
    memcpy(out + len, r->text, t);
    len += t;
    if (r->mark != '+') {
      out[len++] = '\n';
    }
    out[len] = '\0';
  }
  return out;
}

// Number of descriptors open in this process (probing 0..limit).
static inline int open_fd_count(int limit) {
  int n = 0;
  for (int fd = 0; fd < limit; ++fd) {
    if (fcntl(fd, F_GETFD) != -1) {
      ++n;
    }
  }
  return n;
}

#endif
