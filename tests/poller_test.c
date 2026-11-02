// Every backend this platform has must behave identically: level-triggered
// readiness, end-of-file reported as readable, and correct bookkeeping. Only
// select may refuse descriptors at or above FD_SETSIZE.

#include "logmux/poller.h"

#include <errno.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <unistd.h>

#include "check.h"

static int ready_fds(lm_poller* p, int timeout_ms, int* out, int max) {
  lm_event events[16];
  const int n = lm_poller_wait(p, timeout_ms, events, 16);
  for (int i = 0; i < n && i < max; ++i) {
    out[i] = events[i].fd;
  }
  return n;
}

static void test_readiness(lm_backend b) {
  lm_poller* p = lm_poller_create(b);
  CHECK(p != NULL);
  int a[2];
  int c[2];
  CHECK(pipe(a) == 0 && pipe(c) == 0);
  int tag_a = 1;
  int tag_c = 2;
  CHECK(lm_poller_add(p, a[0], &tag_a) == 0);
  CHECK(lm_poller_add(p, c[0], &tag_c) == 0);
  CHECK(lm_poller_add(p, a[0], &tag_a) == -1 && errno == EEXIST);

  int fds[4];
  CHECK_EQ_U(ready_fds(p, 0, fds, 4), 0);  // nothing written yet

  CHECK(write(a[1], "xy", 2) == 2);
  lm_event ev[4];
  CHECK_EQ_U(lm_poller_wait(p, 1000, ev, 4), 1);
  CHECK(ev[0].fd == a[0] && ev[0].tag == &tag_a);

  // Level-triggered: still ready until drained.
  CHECK_EQ_U(ready_fds(p, 0, fds, 4), 1);
  char buf[8];
  CHECK(read(a[0], buf, sizeof buf) == 2);
  CHECK_EQ_U(ready_fds(p, 0, fds, 4), 0);

  // End-of-file is readable, and read then returns 0.
  close(c[1]);
  CHECK_EQ_U(lm_poller_wait(p, 1000, ev, 4), 1);
  CHECK(ev[0].fd == c[0] && ev[0].tag == &tag_c);
  CHECK(read(c[0], buf, sizeof buf) == 0);

  // A removed descriptor is never reported again.
  CHECK(lm_poller_remove(p, c[0]) == 0);
  CHECK(lm_poller_remove(p, c[0]) == -1 && errno == ENOENT);
  CHECK_EQ_U(ready_fds(p, 0, fds, 4), 0);

  // Readiness on several descriptors at once.
  CHECK(lm_poller_add(p, c[0], &tag_c) == 0);  // re-adding after removal works
  CHECK(write(a[1], "z", 1) == 1);
  CHECK_EQ_U(ready_fds(p, 1000, fds, 4), 2);  // a has data, c is at end-of-file

  lm_poller_destroy(p);
  close(a[0]);
  close(a[1]);
  close(c[0]);
}

static void test_wait_timeout(lm_backend b) {
  lm_poller* p = lm_poller_create(b);
  int a[2];
  CHECK(pipe(a) == 0);
  CHECK(lm_poller_add(p, a[0], NULL) == 0);
  lm_event ev[1];
  CHECK_EQ_U(lm_poller_wait(p, 20, ev, 1), 0);
  lm_poller_destroy(p);
  close(a[0]);
  close(a[1]);
}

// select cannot represent descriptors at or above FD_SETSIZE; the others
// must accept them. Skipped if the process may not open that many.
static void test_high_descriptor(lm_backend b) {
  struct rlimit lim;
  getrlimit(RLIMIT_NOFILE, &lim);
  const rlim_t want = FD_SETSIZE + 16;
  if (lim.rlim_cur < want) {
    if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < want) {
      printf("skipping high-descriptor test: hard limit %llu\n", (unsigned long long)lim.rlim_max);
      return;
    }
    lim.rlim_cur = want;
    CHECK(setrlimit(RLIMIT_NOFILE, &lim) == 0);
  }
  int a[2];
  CHECK(pipe(a) == 0);
  const int high = dup2(a[0], FD_SETSIZE + 3);
  CHECK(high == FD_SETSIZE + 3);
  lm_poller* p = lm_poller_create(b);
  const int rc = lm_poller_add(p, high, NULL);
  if (b == LM_SELECT) {
    CHECK(rc == -1 && errno == EINVAL);
  } else {
    CHECK(rc == 0);
    CHECK(write(a[1], "x", 1) == 1);
    lm_event ev[1];
    CHECK_EQ_U(lm_poller_wait(p, 1000, ev, 1), 1);
    CHECK(ev[0].fd == high);
  }
  lm_poller_destroy(p);
  close(high);
  close(a[0]);
  close(a[1]);
}

static void test_names(void) {
  check_context = "names";
  for (lm_backend b = LM_SELECT; b < LM_BACKEND_COUNT; ++b) {
    lm_backend parsed;
    CHECK(lm_backend_parse(lm_backend_name(b), &parsed) && parsed == b);
  }
  lm_backend unused;
  CHECK(!lm_backend_parse("iocp", &unused));
  CHECK(lm_backend_available(lm_backend_default()));
  CHECK(lm_backend_available(LM_SELECT) && lm_backend_available(LM_POLL));
#if defined(__linux__)
  CHECK(lm_backend_default() == LM_EPOLL);
#elif defined(__APPLE__)
  CHECK(lm_backend_default() == LM_KQUEUE);
#endif
}

int main(void) {
  test_names();
  for (lm_backend b = LM_SELECT; b < LM_BACKEND_COUNT; ++b) {
    if (!lm_backend_available(b)) {
      CHECK(lm_poller_create(b) == NULL);
      continue;
    }
    check_context = lm_backend_name(b);
    test_readiness(b);
    test_wait_timeout(b);
    test_high_descriptor(b);
  }
  return check_exit("poller_test");
}
