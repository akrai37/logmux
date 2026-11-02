#ifndef LOGMUX_TESTING_H
#define LOGMUX_TESTING_H

// Deliberately broken collectors, for tests only. Each reproduces a classic
// bug in programs that collect child output, so the tests can prove their
// checks catch it.

#include "logmux/collector.h"

typedef enum {
  // All pipes are created before any child is forked, without close-on-
  // exec. Every child then inherits every other child's pipe write ends,
  // so a child's end-of-file arrives only once all its siblings have exited
  // too: its final unterminated line is held back by an unrelated sibling.
  LM_BROKEN_NAIVE_PIPES,
  // No event loop: each child's stdout is read to end-of-file with blocking
  // reads before its stderr. A child that fills its stderr pipe before
  // closing stdout deadlocks with the collector.
  LM_BROKEN_SEQUENTIAL_READS,
  // Each read is written out as a record as it arrives, without reassembling
  // lines, so lines that span reads are torn.
  LM_BROKEN_NO_REASSEMBLY,
} lm_broken;

int lm_run_broken(lm_broken kind, const lm_config* config, const lm_child_spec* children, size_t n,
                  lm_child_result* results, lm_stats* stats);

#endif
