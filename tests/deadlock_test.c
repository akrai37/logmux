// A collector that reads each child's stdout to the end before its stderr
// deadlocks on a child that fills its stderr pipe first: the child blocks
// writing stderr while the collector blocks reading stdout. logmux must
// finish; the sequential collector must be found stuck.
//
// A deadlocked run never returns, so each run happens in a child process
// that the parent kills after a timeout calibrated from logmux's own run.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "check.h"
#include "logmux/testing.h"

typedef enum { RUN_FINISHED, RUN_FAILED, RUN_HUNG } run_outcome;

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static run_outcome run_in_child(bool broken, double timeout_seconds) {
  const pid_t pid = fork();
  if (pid < 0) {
    return RUN_FAILED;
  }
  if (pid == 0) {
    static char child_path[] = LOGMUX_CHILD;
    char* const argv[] = {child_path, "--id",         "1", "--lines", "100",
                          "--mode",   "stall-stderr", NULL};
    const lm_child_spec spec = {"staller", argv};
    lm_config c = {.backend = lm_backend_default(), .out_fd = open("/dev/null", O_WRONLY)};
    const int rc = broken ? lm_run_broken(LM_BROKEN_SEQUENTIAL_READS, &c, &spec, 1, NULL, NULL)
                          : lm_run(&c, &spec, 1, NULL, NULL);
    _exit(rc == 0 ? 0 : 1);
  }
  const struct timespec tick = {0, 20 * 1000 * 1000};
  const double deadline = now_seconds() + timeout_seconds;
  while (now_seconds() < deadline) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? RUN_FINISHED : RUN_FAILED;
    }
    nanosleep(&tick, NULL);
  }
  // Kill the whole group: the stuck collector and the child it started.
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  return RUN_HUNG;
}

int main(void) {
  check_context = "logmux";
  const double begin = now_seconds();
  const run_outcome correct = run_in_child(false, 60.0);
  const double correct_seconds = now_seconds() - begin;
  CHECK(correct == RUN_FINISHED);

  check_context = "negative control: sequential blocking reads";
  const double limit = correct_seconds * 5 > 3.0 ? correct_seconds * 5 : 3.0;
  const run_outcome sequential = run_in_child(true, limit);
  CHECK(sequential == RUN_HUNG);
  if (sequential == RUN_HUNG) {
    printf(
        "logmux finished in %.2f s; the sequential collector was still stuck after %.1f s, "
        "as expected\n",
        correct_seconds, limit);
  }
  return check_exit("deadlock_test");
}
