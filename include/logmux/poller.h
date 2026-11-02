#ifndef LOGMUX_POLLER_H
#define LOGMUX_POLLER_H

#include <stdbool.h>
#include <stddef.h>

// Readiness notification for many file descriptors, behind one interface so
// the backends can be swapped and compared:
//
//   select  POSIX. Rebuilds and scans a bitmap of every watched descriptor on
//           each wait, and cannot watch descriptors numbered FD_SETSIZE
//           (usually 1024) or above.
//   poll    POSIX. No descriptor limit, but the kernel still checks every
//           watched descriptor on each wait, and so does the caller.
//   epoll   Linux. Interest is registered once; a wait returns only the
//           descriptors that are ready.
//   kqueue  macOS and the BSDs. Same model as epoll.
//
// All backends are level-triggered: a descriptor with unread data is
// reported on every wait until it is drained, and end-of-file or hangup is
// reported as readable (the following read returns 0).

typedef enum {
  LM_SELECT,
  LM_POLL,
  LM_EPOLL,
  LM_KQUEUE,
} lm_backend;

#define LM_BACKEND_COUNT 4

typedef struct {
  int fd;
  void* tag;  // the value given to lm_poller_add
} lm_event;

typedef struct lm_poller lm_poller;

const char* lm_backend_name(lm_backend backend);
bool lm_backend_parse(const char* name, lm_backend* out);
// Whether the backend exists on this platform.
bool lm_backend_available(lm_backend backend);
// epoll on Linux, kqueue on macOS: the scalable one.
lm_backend lm_backend_default(void);

// NULL if the backend is unavailable or allocation fails.
lm_poller* lm_poller_create(lm_backend backend);
void lm_poller_destroy(lm_poller* poller);

// Watch `fd` for readability. Returns 0, or -1 with errno set (EINVAL for a
// descriptor select cannot represent, EEXIST if already watched).
int lm_poller_add(lm_poller* poller, int fd, void* tag);
// Stop watching `fd`. Must be called before `fd` is closed.
int lm_poller_remove(lm_poller* poller, int fd);

// Waits up to `timeout_ms` (-1 = no limit) and fills `events` with up to
// `max` ready descriptors. Returns how many, 0 on timeout, or -1 with errno
// set (EINTR when a signal arrived).
int lm_poller_wait(lm_poller* poller, int timeout_ms, lm_event* events, size_t max);

#endif
