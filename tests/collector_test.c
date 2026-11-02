// The collector end to end: framing, streams, exit statuses, timeouts,
// shutdown and descriptor hygiene, for every backend this platform has.

#include "logmux/collector.h"

#include <signal.h>
#include <string.h>

#include "check.h"
#include "harness.h"

static lm_backend backend;

#define SH(name, script)            \
  {                                 \
    name, (char* const[]) {         \
      "/bin/sh", "-c", script, NULL \
    }                               \
  }

static lm_config config(void) {
  return (lm_config){.backend = backend};
}

static const record* find_record(const log_view* log, const char* name, const char* stream,
                                 const char* text_prefix) {
  for (size_t i = 0; i < log->count; ++i) {
    const record* r = &log->records[i];
    if (strcmp(r->name, name) == 0 && strcmp(r->stream, stream) == 0 &&
        strncmp(r->text, text_prefix, strlen(text_prefix)) == 0) {
      return r;
    }
  }
  return NULL;
}

static void check_lines(const log_view* log, const char* name, const char* stream,
                        const char* expected) {
  char* got = lines_of(log, name, stream);
  CHECK(strcmp(got, expected) == 0);
  if (strcmp(got, expected) != 0) {
    fprintf(stderr, "  %s %s: got [%s] expected [%s]\n", name, stream, got, expected);
  }
  free(got);
}

static void test_streams_and_statuses(void) {
  const lm_child_spec specs[] = {
      SH("a", "echo one; echo two; echo oops >&2"),
      SH("b", "printf 'x\\ny\\n'; exit 3"),
      SH("c", "echo before; kill -KILL $$"),
  };
  lm_child_result r[3];
  int rc;
  log_view log = run_and_capture(config(), specs, 3, r, NULL, &rc);
  CHECK_EQ_U(rc, 0);
  CHECK(log.well_formed);
  check_lines(&log, "a", "out", "one\ntwo\n");
  check_lines(&log, "a", "err", "oops\n");
  check_lines(&log, "b", "out", "x\ny\n");
  check_lines(&log, "c", "out", "before\n");
  CHECK(r[0].exited && r[0].exit_code == 0);
  CHECK(r[1].exited && r[1].exit_code == 3);
  CHECK(r[2].signaled && r[2].term_signal == SIGKILL);
  CHECK_EQ_U(r[0].lines[LM_STDOUT], 2);
  CHECK_EQ_U(r[0].lines[LM_STDERR], 1);
  CHECK(find_record(&log, "b", "sys", "exited with status 3") != NULL);
  CHECK(find_record(&log, "c", "sys", "killed by signal 9") != NULL);
  log_free(&log);
}

static void test_missing_newline_and_long_lines(void) {
  // max_line 10: 25 chars -> 10 '+' 10 '+' 5 ':'; exactly 10 -> one ':';
  // 20 chars and no newline at the end -> 10 '+' then 10 '~'.
  const lm_child_spec specs[] = {
      SH("long", "printf 'abcdefghijKLMNOPQRSTuvwxy\\n0123456789\\n'"),
      SH("tail", "printf 'ABCDEFGHIJklmnopqrst'"),
      SH("empty", "true"),
  };
  lm_config c = config();
  c.max_line = 10;
  lm_child_result r[3];
  int rc;
  log_view log = run_and_capture(c, specs, 3, r, NULL, &rc);
  CHECK(log.well_formed);
  check_lines(&log, "long", "out", "abcdefghijKLMNOPQRSTuvwxy\n0123456789\n");
  check_lines(&log, "tail", "out", "ABCDEFGHIJklmnopqrst\n");
  const record* piece = find_record(&log, "long", "out", "abcdefghij");
  CHECK(piece != NULL && piece->mark == '+' && strlen(piece->text) == 10);
  const record* exact = find_record(&log, "long", "out", "0123456789");
  CHECK(exact != NULL && exact->mark == ':');
  const record* last = find_record(&log, "tail", "out", "klmnopqrst");
  CHECK(last != NULL && last->mark == '~');
  CHECK_EQ_U(r[0].lines[LM_STDOUT], 2);
  CHECK_EQ_U(r[0].fragments[LM_STDOUT], 2);
  CHECK_EQ_U(r[1].fragments[LM_STDOUT], 2);  // one '+' and the final '~'
  CHECK_EQ_U(r[2].lines[LM_STDOUT], 0);
  log_free(&log);
}

static void test_command_not_found(void) {
  const lm_child_spec specs[] = {{"ghost", (char* const[]){"logmux-no-such-command", NULL}}};
  lm_child_result r[1];
  int rc;
  log_view log = run_and_capture(config(), specs, 1, r, NULL, &rc);
  CHECK(r[0].exited && r[0].exit_code == 127);
  CHECK(find_record(&log, "ghost", "err", "logmux: cannot run logmux-no-such-command") != NULL);
  log_free(&log);
}

static void test_hang_timeout_and_escalation(void) {
  const lm_child_spec specs[] = {
      SH("quiet", "echo hi; sleep 30"),
      SH("stubborn", "trap '' TERM; echo ignoring; sleep 30"),
      // The shell exits at once but a background process keeps the pipe
      // open; only signalling the process group ends it.
      SH("orphan", "sleep 30 & echo parent exits"),
  };
  lm_config c = config();
  c.hang_timeout_ms = 200;
  c.kill_grace_ms = 200;
  lm_child_result r[3];
  lm_stats st;
  int rc;
  log_view log = run_and_capture(c, specs, 3, r, &st, &rc);
  CHECK_EQ_U(rc, 0);
  CHECK(st.seconds < 5);
  CHECK(r[0].timed_out && r[0].signaled && r[0].term_signal == SIGTERM);
  CHECK(r[1].timed_out && r[1].signaled && r[1].term_signal == SIGKILL);
  CHECK(r[2].exited && r[2].exit_code == 0);
  CHECK(find_record(&log, "stubborn", "sys", "still running") != NULL);
  log_free(&log);
}

// A child sends SIGTERM to logmux itself: logmux must forward it, drain the
// output already written and return normally. The bystander creates a
// marker file once its line is in the pipe, and the trigger waits for it,
// so the test does not depend on which child runs first.
static void test_shutdown_on_signal(void) {
  char dir[] = "/tmp/logmux-test-XXXXXX";
  CHECK(mkdtemp(dir) != NULL);
  char trigger[256];
  char bystander[256];
  snprintf(trigger, sizeof trigger,
           "until [ -f %s/ready ]; do sleep 0.01; done; echo stopping; kill -TERM $PPID; "
           "sleep 30",
           dir);
  snprintf(bystander, sizeof bystander, "echo still here; touch %s/ready; sleep 30", dir);
  const lm_child_spec specs[] = {
      {"trigger", (char* const[]){"/bin/sh", "-c", trigger, NULL}},
      {"bystander", (char* const[]){"/bin/sh", "-c", bystander, NULL}},
  };
  lm_child_result r[2];
  lm_stats st;
  int rc;
  log_view log = run_and_capture(config(), specs, 2, r, &st, &rc);
  CHECK_EQ_U(rc, 0);
  CHECK(st.seconds < 5);
  CHECK(r[0].signaled && r[1].signaled);
  check_lines(&log, "bystander", "out", "still here\n");
  CHECK(find_record(&log, "bystander", "sys", "shutting down") != NULL);
  log_free(&log);
  char ready[300];
  snprintf(ready, sizeof ready, "%s/ready", dir);
  unlink(ready);
  rmdir(dir);
}

static void test_no_descriptor_leaks(void) {
  const int before = open_fd_count(4096);
  const lm_child_spec specs[] = {SH("a", "echo a"), SH("b", "echo b >&2; exit 1"),
                                 SH("c", "sleep 30")};
  lm_config c = config();
  c.hang_timeout_ms = 100;
  c.kill_grace_ms = 100;
  int rc;
  log_view log = run_and_capture(c, specs, 3, NULL, NULL, &rc);
  log_free(&log);
  CHECK_EQ_U(open_fd_count(4096), before);
}

static void test_many_children(void) {
  enum { N = 100 };
  lm_child_spec specs[N];
  char names[N][16];
  for (int i = 0; i < N; ++i) {
    snprintf(names[i], sizeof names[i], "c%d", i);
    specs[i] = (lm_child_spec)SH(names[i], "for i in 1 2 3; do echo line $i; done");
  }
  lm_child_result r[N];
  int rc;
  log_view log = run_and_capture(config(), specs, N, r, NULL, &rc);
  CHECK(log.well_formed);
  for (int i = 0; i < N; ++i) {
    check_lines(&log, names[i], "out", "line 1\nline 2\nline 3\n");
    CHECK(r[i].exited && r[i].exit_code == 0);
  }
  log_free(&log);
}

int main(void) {
  for (lm_backend b = LM_SELECT; b < LM_BACKEND_COUNT; ++b) {
    if (!lm_backend_available(b)) {
      continue;
    }
    backend = b;
    check_context = lm_backend_name(b);
    test_streams_and_statuses();
    test_missing_newline_and_long_lines();
    test_command_not_found();
    test_hang_timeout_and_escalation();
    test_shutdown_on_signal();
    test_no_descriptor_leaks();
    test_many_children();
  }
  return check_exit("collector_test");
}
