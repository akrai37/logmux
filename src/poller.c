#include "logmux/poller.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

// Descriptors the poller watches, with their tags, in insertion order. The
// select backend scans this list on every wait.
typedef struct {
  int fd;
  void* tag;
} watch;

struct lm_poller {
  lm_backend backend;
  watch* watches;
  size_t count;
  size_t capacity;
};

static const char* const kBackendNames[LM_BACKEND_COUNT] = {"select", "poll", "epoll", "kqueue"};

const char* lm_backend_name(lm_backend backend) {
  return (unsigned)backend < LM_BACKEND_COUNT ? kBackendNames[backend] : "?";
}

bool lm_backend_parse(const char* name, lm_backend* out) {
  for (unsigned i = 0; i < LM_BACKEND_COUNT; ++i) {
    if (strcmp(name, kBackendNames[i]) == 0) {
      *out = (lm_backend)i;
      return true;
    }
  }
  return false;
}

bool lm_backend_available(lm_backend backend) {
  return backend == LM_SELECT;
}

lm_backend lm_backend_default(void) {
  return LM_SELECT;
}

lm_poller* lm_poller_create(lm_backend backend) {
  if (!lm_backend_available(backend)) {
    return NULL;
  }
  lm_poller* p = calloc(1, sizeof *p);
  if (p != NULL) {
    p->backend = backend;
  }
  return p;
}

void lm_poller_destroy(lm_poller* p) {
  if (p != NULL) {
    free(p->watches);
    free(p);
  }
}

static ptrdiff_t find(const lm_poller* p, int fd) {
  for (size_t i = 0; i < p->count; ++i) {
    if (p->watches[i].fd == fd) {
      return (ptrdiff_t)i;
    }
  }
  return -1;
}

int lm_poller_add(lm_poller* p, int fd, void* tag) {
  if (fd < 0 || (p->backend == LM_SELECT && fd >= FD_SETSIZE)) {
    errno = EINVAL;
    return -1;
  }
  if (find(p, fd) >= 0) {
    errno = EEXIST;
    return -1;
  }
  if (p->count == p->capacity) {
    const size_t capacity = p->capacity == 0 ? 16 : p->capacity * 2;
    watch* grown = realloc(p->watches, capacity * sizeof *grown);
    if (grown == NULL) {
      return -1;
    }
    p->watches = grown;
    p->capacity = capacity;
  }
  p->watches[p->count++] = (watch){fd, tag};
  return 0;
}

int lm_poller_remove(lm_poller* p, int fd) {
  const ptrdiff_t i = find(p, fd);
  if (i < 0) {
    errno = ENOENT;
    return -1;
  }
  p->watches[i] = p->watches[--p->count];
  return 0;
}

// select: the fd_set is rebuilt from the watch list and then scanned, so
// every wait costs time proportional to everything watched, ready or not.
static int wait_select(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  fd_set readable;
  FD_ZERO(&readable);
  int highest = -1;
  for (size_t i = 0; i < p->count; ++i) {
    FD_SET(p->watches[i].fd, &readable);
    if (p->watches[i].fd > highest) {
      highest = p->watches[i].fd;
    }
  }
  struct timeval tv;
  struct timeval* timeout = NULL;
  if (timeout_ms >= 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    timeout = &tv;
  }
  const int ready = select(highest + 1, &readable, NULL, NULL, timeout);
  if (ready <= 0) {
    return ready;
  }
  int n = 0;
  for (size_t i = 0; i < p->count && (size_t)n < max; ++i) {
    if (FD_ISSET(p->watches[i].fd, &readable)) {
      events[n++] = (lm_event){p->watches[i].fd, p->watches[i].tag};
    }
  }
  return n;
}

int lm_poller_wait(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  switch (p->backend) {
    case LM_SELECT:
      return wait_select(p, timeout_ms, events, max);
    case LM_POLL:
    case LM_EPOLL:
    case LM_KQUEUE:
      break;
  }
  errno = ENOSYS;
  return -1;
}
