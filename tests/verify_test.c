// Every byte the children write must reach the log exactly once, intact and
// in order, whatever the backend and however hostile the child: flooding,
// trickling one byte at a time, crashing mid-line, closing stdout early,
// hanging, or filling stderr before stdout. The verifier itself must catch
// corruption, and two deliberately broken collectors must be caught.

#include "verify.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "logmux/testing.h"

#define MAX_LEN 600   // longest payload logmux-child writes
#define MAX_LINE 256  // collector limit: longer lines come out in pieces

// argv for one logmux-child, with its own copies of every string.
typedef struct {
  char* argv[16];
  char storage[8][64];
} child_command;

static void make_command(child_command* c, const child_expect* e, const char* chunk,
                         const char* delay_us) {
  snprintf(c->storage[0], 64, "%u", e->id);
  snprintf(c->storage[1], 64, "%llu", e->lines);
  snprintf(c->storage[2], 64, "%llu", e->seed);
  snprintf(c->storage[3], 64, "%zu", e->max_len);
  snprintf(c->storage[4], 64, "%s", e->mode);
  snprintf(c->storage[5], 64, "%s", chunk);
  snprintf(c->storage[6], 64, "%s", delay_us);
  static char child_path[] = LOGMUX_CHILD;
  char* const argv[] = {child_path,    "--id",       c->storage[0], "--lines",
                        c->storage[1], "--mode",     c->storage[4], "--seed",
                        c->storage[2], "--max-len",  c->storage[3], "--chunk",
                        c->storage[5], "--delay-us", c->storage[6], NULL};
  memcpy(c->argv, argv, sizeof argv);
}

static const char* const kModes[] = {"normal",     "both",        "flood", "trickle",     "crash",
                                     "no-newline", "early-close", "hang",  "stall-stderr"};
#define MODES (sizeof kModes / sizeof kModes[0])
#define CHILDREN (2 * MODES)

static unsigned long long lines_for(const char* mode) {
  if (strcmp(mode, "trickle") == 0) {
    return 120;  // one byte per write
  }
  if (strcmp(mode, "flood") == 0) {
    return 20000;
  }
  return 1500;
}

// Builds CHILDREN children cycling through every mode.
static void make_children(uint64_t seed, child_expect* expect, child_command* cmds,
                          lm_child_spec* specs, char names[][32]) {
  for (unsigned i = 0; i < CHILDREN; ++i) {
    const char* mode = kModes[i % MODES];
    snprintf(names[i], 32, "%s-%u", mode, i);
    expect[i] = (child_expect){names[i], i + 1, mode, lines_for(mode), seed, MAX_LEN};
    make_command(&cmds[i], &expect[i], "700", "0");
    specs[i] = (lm_child_spec){names[i], cmds[i].argv};
  }
}

static lm_config config(lm_backend backend) {
  // The hang child is killed after 1.5 s of silence; everything else
  // writes continuously and is never idle that long.
  return (lm_config){
      .backend = backend, .max_line = MAX_LINE, .hang_timeout_ms = 1500, .kill_grace_ms = 500};
}

static void test_exact_collection(lm_backend backend, uint64_t seed) {
  child_expect expect[CHILDREN];
  child_command cmds[CHILDREN];
  lm_child_spec specs[CHILDREN];
  char names[CHILDREN][32];
  make_children(seed, expect, cmds, specs, names);

  lm_child_result r[CHILDREN];
  lm_stats st;
  int rc;
  log_view log = run_and_capture(config(backend), specs, CHILDREN, r, &st, &rc);
  CHECK_EQ_U(rc, 0);
  CHECK(log.well_formed);
  for (unsigned i = 0; i < CHILDREN; ++i) {
    char why[256];
    const bool ok = verify_child(&log, &expect[i], why, sizeof why);
    CHECK(ok);
    if (!ok) {
      fprintf(stderr, "  seed %llu: %s\n", (unsigned long long)seed, why);
    }
    const char* mode = expect[i].mode;
    if (strcmp(mode, "crash") == 0) {
      CHECK(r[i].signaled && r[i].term_signal == SIGABRT);
    } else if (strcmp(mode, "hang") == 0) {
      CHECK(r[i].timed_out && r[i].signaled);
    } else {
      CHECK(r[i].exited && r[i].exit_code == 0);
    }
  }
  log_free(&log);
}

// The verifier must notice a dropped record, two swapped records, and one
// changed byte. Otherwise a pass would mean nothing.
static void test_verifier_detects_corruption(void) {
  check_context = "verifier";
  child_expect e = {"v", 1, "normal", 50, 7, MAX_LEN};
  child_command cmd;
  make_command(&cmd, &e, "64", "0");
  const lm_child_spec spec = {"v", cmd.argv};
  int rc;
  log_view log = run_and_capture(config(lm_backend_default()), &spec, 1, NULL, NULL, &rc);
  char why[256];
  CHECK(verify_child(&log, &e, why, sizeof why));

  // Find two adjacent output records of the child.
  size_t a = 0;
  while (a < log.count && strcmp(log.records[a].stream, "out") != 0) {
    ++a;
  }
  CHECK(a + 1 < log.count);

  record saved = log.records[a];
  log.records[a].stream[0] = 'x';  // dropped from the out stream
  CHECK(!verify_child(&log, &e, why, sizeof why));
  log.records[a] = saved;

  record tmp = log.records[a];  // reordered
  log.records[a] = log.records[a + 1];
  log.records[a + 1] = tmp;
  CHECK(!verify_child(&log, &e, why, sizeof why));
  log.records[a + 1] = log.records[a];
  log.records[a] = tmp;

  const size_t len = strlen(log.records[a].text);
  if (len > 0) {
    log.records[a].text[len - 1] ^= 1;  // one byte changed
    CHECK(!verify_child(&log, &e, why, sizeof why));
    log.records[a].text[len - 1] ^= 1;
  }
  CHECK(verify_child(&log, &e, why, sizeof why));  // restored
  log_free(&log);
}

// Writing each read as a record tears every line that spans two reads.
static void test_no_reassembly_is_caught(void) {
  check_context = "negative control: no reassembly";
  child_expect expect[2] = {{"trickle", 1, "trickle", 60, 3, MAX_LEN},
                            {"flood", 2, "flood", 5000, 3, MAX_LEN}};
  child_command cmds[2];
  lm_child_spec specs[2];
  for (int i = 0; i < 2; ++i) {
    make_command(&cmds[i], &expect[i], "700", "0");
    specs[i] = (lm_child_spec){expect[i].name, cmds[i].argv};
  }
  lm_config c = config(lm_backend_default());
  c.out_fd = -1;
  FILE* tmp = tmpfile();
  c.out_fd = fileno(tmp);
  CHECK_EQ_U(lm_run_broken(LM_BROKEN_NO_REASSEMBLY, &c, specs, 2, NULL, NULL), 0);
  fseek(tmp, 0, SEEK_END);
  const long size = ftell(tmp);
  char* raw = malloc((size_t)size + 1);
  rewind(tmp);
  const size_t got = fread(raw, 1, (size_t)size, tmp);
  fclose(tmp);
  log_view log = log_parse(raw, got);
  free(raw);
  char why[256];
  const bool trickle_ok = verify_child(&log, &expect[0], why, sizeof why);
  CHECK(!trickle_ok);
  if (!trickle_ok) {
    printf("no-reassembly collector caught: %s\n", why);
  }
  log_free(&log);
}

// With every pipe created up front and inheritable, a child's end-of-file
// waits for its siblings: its unterminated last line appears only when an
// unrelated, long-running sibling exits.
static double final_fragment_time(bool broken) {
  child_expect e = {"quick", 1, "no-newline", 5, 9, 64};
  child_command cmd;
  make_command(&cmd, &e, "64", "0");
  const lm_child_spec specs[] = {
      {"quick", cmd.argv},
      {"slow", (char* const[]){"/bin/sh", "-c", "sleep 2", NULL}},
  };
  lm_config c = config(lm_backend_default());
  FILE* tmp = tmpfile();
  c.out_fd = fileno(tmp);
  const int rc = broken ? lm_run_broken(LM_BROKEN_NAIVE_PIPES, &c, specs, 2, NULL, NULL)
                        : lm_run(&c, specs, 2, NULL, NULL);
  CHECK_EQ_U(rc, 0);
  fseek(tmp, 0, SEEK_END);
  const long size = ftell(tmp);
  char* raw = malloc((size_t)size + 1);
  rewind(tmp);
  const size_t got = fread(raw, 1, (size_t)size, tmp);
  fclose(tmp);
  log_view log = log_parse(raw, got);
  free(raw);
  double t = -1;
  for (size_t i = 0; i < log.count; ++i) {
    if (strcmp(log.records[i].name, "quick") == 0 && log.records[i].mark == '~') {
      t = log.records[i].seconds;
    }
  }
  char why[256];
  CHECK(verify_child(&log, &e, why, sizeof why));  // late, but still correct
  log_free(&log);
  return t;
}

static void test_naive_pipes_are_caught(void) {
  check_context = "negative control: naive pipes";
  const double correct = final_fragment_time(false);
  const double naive = final_fragment_time(true);
  CHECK(correct >= 0 && correct < 1.0);
  CHECK(naive >= 1.5);
  printf(
      "final line of a finished child: %.3f s with logmux's pipes, %.3f s with naive "
      "pipes (held until its sibling exited)\n",
      correct, naive);
}

int main(void) {
  for (lm_backend b = LM_SELECT; b < LM_BACKEND_COUNT; ++b) {
    if (!lm_backend_available(b)) {
      continue;
    }
    check_context = lm_backend_name(b);
    for (uint64_t seed = 1; seed <= 2; ++seed) {
      test_exact_collection(b, seed);
    }
  }
  test_verifier_detects_corruption();
  test_no_reassembly_is_caught();
  test_naive_pipes_are_caught();
  return check_exit("verify_test");
}
