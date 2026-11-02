#ifndef LOGMUX_COLLECTOR_H
#define LOGMUX_COLLECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "logmux/poller.h"

// Runs child processes and merges their stdout and stderr into one log.
//
// Each child gets a pipe per stream and its own process group, with stdin
// from /dev/null. One event loop reads whichever pipes are ready, never
// blocking on any single child, and writes one record per line:
//
//   <seconds since start> <child name> <stream><mark> <text>
//
//   stream  out | err | sys (sys: logmux's own notes about the child)
//   mark    ':' a complete line
//           '+' a piece of a line longer than max_line; more follows
//           '~' the final bytes of a stream that did not end in a newline
//
// e.g.  0.004211 worker-2 out: request 17 done
//
// Lines from different children never mix: a line is written only once it
// is complete (or cut at max_line), and each record is written whole.
//
// A child that produces no output for hang_timeout_ms is sent SIGTERM, then
// SIGKILL after kill_grace_ms, to its whole process group. On SIGINT or
// SIGTERM, logmux forwards SIGTERM to every child and keeps collecting
// until all output has been drained and every child reaped.
//
// lm_run installs signal handlers for its duration and is not reentrant.

typedef struct {
  const char* name;   // label in the log; no whitespace
  char* const* argv;  // NULL-terminated; argv[0] is looked up in PATH
} lm_child_spec;

typedef struct {
  lm_backend backend;
  int out_fd;           // the merged log is written here
  size_t max_line;      // longer lines are cut into pieces (0 = 4096)
  int hang_timeout_ms;  // silence that counts as hung; 0 = never
  int kill_grace_ms;    // SIGTERM -> SIGKILL delay (0 = 2000)
} lm_config;

enum { LM_STDOUT, LM_STDERR };

typedef struct {
  pid_t pid;
  bool spawned;
  bool exited;  // exit_code is valid
  int exit_code;
  bool signaled;  // term_signal is valid
  int term_signal;
  bool timed_out;  // logmux killed it for hanging
  uint64_t bytes[2];
  uint64_t lines[2];      // complete lines plus final fragments
  uint64_t fragments[2];  // records marked '+' or '~'
} lm_child_result;

typedef struct {
  double seconds;
  uint64_t wakeups;    // waits that returned ready descriptors
  uint64_t events;     // ready descriptors handled
  uint64_t reads;      // reads that returned data
  uint64_t bytes;      // child output collected
  uint64_t records;    // records written to the log
  double cpu_seconds;  // logmux's own user + system time, children excluded
} lm_stats;

// Runs `n` children to completion. `results` (n entries) and `stats` may be
// NULL. Returns 0 once every child has been reaped and its output written,
// or -1 with errno set if logmux itself could not run (a child that fails
// to start is reported in its result and the log instead).
int lm_run(const lm_config* config, const lm_child_spec* children, size_t n,
           lm_child_result* results, lm_stats* stats);

#endif
