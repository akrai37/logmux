#include "logmux/poller.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#define LM_HAVE_EPOLL 1
#endif
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#define LM_HAVE_KQUEUE 1
#endif

struct lm_poller {
  lm_backend backend;
  // Every watched descriptor, in no particular order. select and poll
  // scan this on each wait; epoll and kqueue never need to.
  int* fds;
  size_t count;
  size_t capacity;
  struct pollfd* pfds;  // poll: parallel to fds
  // Tag and position in `fds` by descriptor number, for O(1) lookup.
  void** tag_of;
  size_t* index_of;
  size_t table_size;
  int kernel_fd;  // epoll or kqueue instance, -1 otherwise
  void* scratch;  // epoll/kqueue event buffer
  size_t scratch_count;
};

#define NOT_WATCHED SIZE_MAX

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
  switch (backend) {
    case LM_SELECT:
    case LM_POLL:
      return true;
    case LM_EPOLL:
#ifdef LM_HAVE_EPOLL
      return true;
#else
      return false;
#endif
    case LM_KQUEUE:
#ifdef LM_HAVE_KQUEUE
      return true;
#else
      return false;
#endif
  }
  return false;
}

lm_backend lm_backend_default(void) {
#if defined(LM_HAVE_EPOLL)
  return LM_EPOLL;
#elif defined(LM_HAVE_KQUEUE)
  return LM_KQUEUE;
#else
  return LM_POLL;
#endif
}

lm_poller* lm_poller_create(lm_backend backend) {
  if (!lm_backend_available(backend)) {
    return NULL;
  }
  lm_poller* p = calloc(1, sizeof *p);
  if (p == NULL) {
    return NULL;
  }
  p->backend = backend;
  p->kernel_fd = -1;
#ifdef LM_HAVE_EPOLL
  if (backend == LM_EPOLL) {
    p->kernel_fd = epoll_create1(EPOLL_CLOEXEC);
  }
#endif
#ifdef LM_HAVE_KQUEUE
  if (backend == LM_KQUEUE) {
    p->kernel_fd = kqueue();
    if (p->kernel_fd >= 0) {
      fcntl(p->kernel_fd, F_SETFD, FD_CLOEXEC);
    }
  }
#endif
  if ((backend == LM_EPOLL || backend == LM_KQUEUE) && p->kernel_fd < 0) {
    free(p);
    return NULL;
  }
  return p;
}

void lm_poller_destroy(lm_poller* p) {
  if (p == NULL) {
    return;
  }
  if (p->kernel_fd >= 0) {
    close(p->kernel_fd);
  }
  free(p->fds);
  free(p->pfds);
  free(p->tag_of);
  free(p->index_of);
  free(p->scratch);
  free(p);
}

static bool watched(const lm_poller* p, int fd) {
  return (size_t)fd < p->table_size && p->index_of[fd] != NOT_WATCHED;
}

static int grow_table(lm_poller* p, int fd) {
  if ((size_t)fd < p->table_size) {
    return 0;
  }
  size_t size = p->table_size == 0 ? 64 : p->table_size;
  while (size <= (size_t)fd) {
    size *= 2;
  }
  void** tags = realloc(p->tag_of, size * sizeof *tags);
  if (tags == NULL) {
    return -1;
  }
  p->tag_of = tags;
  size_t* index = realloc(p->index_of, size * sizeof *index);
  if (index == NULL) {
    return -1;
  }
  p->index_of = index;
  for (size_t i = p->table_size; i < size; ++i) {
    p->index_of[i] = NOT_WATCHED;
  }
  p->table_size = size;
  return 0;
}

static int grow_list(lm_poller* p) {
  if (p->count < p->capacity) {
    return 0;
  }
  const size_t capacity = p->capacity == 0 ? 16 : p->capacity * 2;
  int* fds = realloc(p->fds, capacity * sizeof *fds);
  if (fds == NULL) {
    return -1;
  }
  p->fds = fds;
  struct pollfd* pfds = realloc(p->pfds, capacity * sizeof *pfds);
  if (pfds == NULL) {
    return -1;
  }
  p->pfds = pfds;
  p->capacity = capacity;
  return 0;
}

static int kernel_register(lm_poller* p, int fd, void* tag, bool add) {
#ifdef LM_HAVE_EPOLL
  if (p->backend == LM_EPOLL) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;  // level-triggered
    ev.data.fd = fd;
    return epoll_ctl(p->kernel_fd, add ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, fd, &ev);
  }
#endif
#ifdef LM_HAVE_KQUEUE
  if (p->backend == LM_KQUEUE) {
    struct kevent kev;
    EV_SET(&kev, (uintptr_t)fd, EVFILT_READ, add ? EV_ADD : EV_DELETE, 0, 0, tag);
    return kevent(p->kernel_fd, &kev, 1, NULL, 0, NULL);
  }
#endif
  (void)p;
  (void)fd;
  (void)tag;
  (void)add;
  return 0;
}

int lm_poller_add(lm_poller* p, int fd, void* tag) {
  if (fd < 0 || (p->backend == LM_SELECT && fd >= FD_SETSIZE)) {
    errno = EINVAL;
    return -1;
  }
  if (watched(p, fd)) {
    errno = EEXIST;
    return -1;
  }
  if (grow_table(p, fd) != 0 || grow_list(p) != 0 || kernel_register(p, fd, tag, true) != 0) {
    return -1;
  }
  p->tag_of[fd] = tag;
  p->index_of[fd] = p->count;
  p->fds[p->count] = fd;
  p->pfds[p->count] = (struct pollfd){.fd = fd, .events = POLLIN};
  ++p->count;
  return 0;
}

int lm_poller_remove(lm_poller* p, int fd) {
  if (!watched(p, fd)) {
    errno = ENOENT;
    return -1;
  }
  kernel_register(p, fd, NULL, false);
  // Move the last entry into the hole.
  const size_t i = p->index_of[fd];
  const size_t last = --p->count;
  p->fds[i] = p->fds[last];
  p->pfds[i] = p->pfds[last];
  p->index_of[p->fds[i]] = i;
  p->index_of[fd] = NOT_WATCHED;
  return 0;
}

// --- Waiting --------------------------------------------------------------------

// select: the fd_set is rebuilt from the list and then scanned, so every
// wait costs time proportional to everything watched, ready or not.
static int wait_select(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  fd_set readable;
  FD_ZERO(&readable);
  int highest = -1;
  for (size_t i = 0; i < p->count; ++i) {
    FD_SET(p->fds[i], &readable);
    if (p->fds[i] > highest) {
      highest = p->fds[i];
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
    if (FD_ISSET(p->fds[i], &readable)) {
      events[n++] = (lm_event){p->fds[i], p->tag_of[p->fds[i]]};
    }
  }
  return n;
}

// poll: no descriptor limit, but the kernel checks every entry and the
// result is found by scanning the whole array again.
static int wait_poll(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  const int ready = poll(p->pfds, (nfds_t)p->count, timeout_ms);
  if (ready <= 0) {
    return ready;
  }
  int n = 0;
  for (size_t i = 0; i < p->count && (size_t)n < max; ++i) {
    if (p->pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
      events[n++] = (lm_event){p->pfds[i].fd, p->tag_of[p->pfds[i].fd]};
    }
  }
  return n;
}

static int reserve_scratch(lm_poller* p, size_t count, size_t size) {
  if (p->scratch_count >= count) {
    return 0;
  }
  void* grown = realloc(p->scratch, count * size);
  if (grown == NULL) {
    return -1;
  }
  p->scratch = grown;
  p->scratch_count = count;
  return 0;
}

#ifdef LM_HAVE_EPOLL
// epoll: the kernel keeps the interest list and returns only ready
// descriptors, so a wait costs time proportional to what is ready.
static int wait_epoll(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  if (reserve_scratch(p, max, sizeof(struct epoll_event)) != 0) {
    return -1;
  }
  struct epoll_event* evs = p->scratch;
  const int ready = epoll_wait(p->kernel_fd, evs, (int)max, timeout_ms);
  for (int i = 0; i < ready; ++i) {
    events[i] = (lm_event){evs[i].data.fd, p->tag_of[evs[i].data.fd]};
  }
  return ready;
}
#endif

#ifdef LM_HAVE_KQUEUE
// kqueue: the same model as epoll. End-of-file arrives as a read event with
// EV_EOF set, after any remaining data.
static int wait_kqueue(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  if (reserve_scratch(p, max, sizeof(struct kevent)) != 0) {
    return -1;
  }
  struct kevent* evs = p->scratch;
  struct timespec ts;
  struct timespec* timeout = NULL;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000;
    timeout = &ts;
  }
  const int ready = kevent(p->kernel_fd, NULL, 0, evs, (int)max, timeout);
  for (int i = 0; i < ready; ++i) {
    events[i] = (lm_event){(int)evs[i].ident, evs[i].udata};
  }
  return ready;
}
#endif

int lm_poller_wait(lm_poller* p, int timeout_ms, lm_event* events, size_t max) {
  switch (p->backend) {
    case LM_SELECT:
      return wait_select(p, timeout_ms, events, max);
    case LM_POLL:
      return wait_poll(p, timeout_ms, events, max);
    case LM_EPOLL:
#ifdef LM_HAVE_EPOLL
      return wait_epoll(p, timeout_ms, events, max);
#else
      break;
#endif
    case LM_KQUEUE:
#ifdef LM_HAVE_KQUEUE
      return wait_kqueue(p, timeout_ms, events, max);
#else
      break;
#endif
  }
  errno = ENOSYS;
  return -1;
}
