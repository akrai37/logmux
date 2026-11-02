#include "logmux/collector.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "logmux/testing.h"

#define READ_CHUNK 65536
#define DEFAULT_MAX_LINE 4096
#define MAX_MAX_LINE (1u << 20)
#define DEFAULT_GRACE_MS 2000
#define MAX_EVENTS 256

static const char* const kStreamNames[2] = {"out", "err"};

// An lm_broken value while lm_run_broken is running, -1 otherwise.
static int g_broken = -1;

typedef struct child child;

typedef struct {
  child* owner;
  int kind;  // LM_STDOUT or LM_STDERR
  int fd;    // -1 once closed
  char* buf;
  size_t len;
} stream;

struct child {
  const lm_child_spec* spec;
  lm_child_result* result;
  bool reaped;
  stream streams[2];
  uint64_t last_output_ns;
  int kill_stage;  // 0 none, 1 SIGTERM sent, 2 SIGKILL sent
  uint64_t term_sent_ns;
};

// --- Signals ------------------------------------------------------------------

// The handlers only record what happened and wake the event loop by writing
// a byte to a pipe it watches (the self-pipe trick); all real work happens in
// the loop, never in a handler.
static int g_wake_fd = -1;
static volatile sig_atomic_t g_shutdown;

static void on_signal(int sig) {
  const int saved = errno;
  if (sig == SIGINT || sig == SIGTERM) {
    g_shutdown = 1;
  }
  if (g_wake_fd >= 0) {
    const char byte = 0;
    (void)!write(g_wake_fd, &byte, 1);  // a full pipe already guarantees a wakeup
  }
  errno = saved;
}

typedef struct {
  struct sigaction chld, intr, term, pipe;
} saved_signals;

static void install_signals(saved_signals* saved) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigaction(SIGCHLD, &sa, &saved->chld);
  sigaction(SIGINT, &sa, &saved->intr);
  sigaction(SIGTERM, &sa, &saved->term);
  // A closed log reader (logmux | head) must not kill logmux silently; the
  // failed write is handled instead.
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, &saved->pipe);
}

static void restore_signals(const saved_signals* saved) {
  sigaction(SIGCHLD, &saved->chld, NULL);
  sigaction(SIGINT, &saved->intr, NULL);
  sigaction(SIGTERM, &saved->term, NULL);
  sigaction(SIGPIPE, &saved->pipe, NULL);
}

// --- Output -------------------------------------------------------------------

typedef struct {
  int fd;
  char* buf;
  size_t len;
  size_t cap;
  bool failed;  // the log can no longer be written
  uint64_t start_ns;
  uint64_t records;
} writer;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void write_all(writer* w, const char* data, size_t len) {
  size_t off = 0;
  while (off < len && !w->failed) {
    const ssize_t n = write(w->fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      w->failed = true;
      break;
    }
    off += (size_t)n;
  }
}

static void flush(writer* w) {
  write_all(w, w->buf, w->len);
  w->len = 0;
}

// Appends one whole record. Records are never split across flushes, so a
// record is always written by a single write call or as a contiguous part
// of one.
static void emit(writer* w, const char* name, const char* stream_name, char mark, const char* text,
                 size_t text_len) {
  char head[160];
  const uint64_t t = now_ns() - w->start_ns;
  const int n = snprintf(head, sizeof head, "%" PRIu64 ".%06" PRIu64 " %s %s%c ", t / 1000000000u,
                         (t / 1000u) % 1000000u, name, stream_name, mark);
  const size_t head_len = n > 0 ? (size_t)n < sizeof head ? (size_t)n : sizeof head - 1 : 0;
  const size_t need = head_len + text_len + 1;
  if (w->len + need > w->cap) {
    flush(w);
  }
  if (need > w->cap) {
    // Larger than the whole buffer: written straight through, still in order
    // and still whole relative to every other record.
    write_all(w, head, head_len);
    write_all(w, text, text_len);
    write_all(w, "\n", 1);
    ++w->records;
    return;
  }
  memcpy(w->buf + w->len, head, head_len);
  memcpy(w->buf + w->len + head_len, text, text_len);
  w->buf[w->len + head_len + text_len] = '\n';
  w->len += need;
  ++w->records;
}

__attribute__((format(printf, 3, 4))) static void note(writer* w, const child* c, const char* fmt,
                                                       ...);

static void note(writer* w, const child* c, const char* fmt, ...) {
  char text[256];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  emit(w, c->spec->name, "sys", ':', text,
       n < 0                     ? 0
       : (size_t)n < sizeof text ? (size_t)n
                                 : sizeof text - 1);
}

// --- Line framing -------------------------------------------------------------

static void record(writer* w, stream* s, char mark, const char* text, size_t len) {
  lm_child_result* r = s->owner->result;
  emit(w, s->owner->spec->name, kStreamNames[s->kind], mark, text, len);
  if (mark != '+') {
    ++r->lines[s->kind];
  }
  if (mark != ':') {
    ++r->fragments[s->kind];
  }
}

// Writes every complete line in the buffer, cuts pieces off any line longer
// than max_line, and keeps the rest for the next read. A piece is cut only
// once more than max_line bytes are waiting without a newline, so a line of
// exactly max_line bytes is never split.
static void frame_lines(writer* w, stream* s, size_t max_line) {
  size_t start = 0;
  for (;;) {
    const size_t waiting = s->len - start;
    const char* nl = memchr(s->buf + start, '\n', waiting);
    const size_t line = nl != NULL ? (size_t)(nl - (s->buf + start)) : waiting;
    if (line > max_line) {
      record(w, s, '+', s->buf + start, max_line);
      start += max_line;
    } else if (nl != NULL) {
      record(w, s, ':', s->buf + start, line);
      start += line + 1;
    } else {
      break;
    }
  }
  memmove(s->buf, s->buf + start, s->len - start);
  s->len -= start;
}

// End of stream: whatever is left had no newline.
static void flush_fragment(writer* w, stream* s, size_t max_line) {
  size_t start = 0;
  while (s->len - start > max_line) {
    record(w, s, '+', s->buf + start, max_line);
    start += max_line;
  }
  if (s->len > start) {
    record(w, s, '~', s->buf + start, s->len - start);
  }
  s->len = 0;
}

// --- Children -----------------------------------------------------------------

static int set_flags(int fd, int fd_flags, int status_flags) {
  if (fd_flags != 0 && fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | fd_flags) != 0) {
    return -1;
  }
  if (status_flags != 0 && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | status_flags) != 0) {
    return -1;
  }
  return 0;
}

// A pipe whose ends are close-on-exec from the start. Without this, every
// child would inherit the write ends of its siblings' pipes, and the parent
// would not see end-of-file on a pipe until every child holding a copy had
// exited.
static int cloexec_pipe(int fds[2]) {
  if (pipe(fds) != 0) {
    return -1;
  }
  if (set_flags(fds[0], FD_CLOEXEC, 0) != 0 || set_flags(fds[1], FD_CLOEXEC, 0) != 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  return 0;
}

static void child_exec(const lm_child_spec* spec, int devnull, int out_w, int err_w) {
  // Own process group, so timeouts and shutdown reach anything it starts.
  setpgid(0, 0);
  // Ignored signals stay ignored across exec; logmux ignores SIGPIPE, which
  // the child must not inherit. Handled signals reset on exec by themselves.
  signal(SIGPIPE, SIG_DFL);
  signal(SIGCHLD, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, NULL);
  // dup2 clears close-on-exec on the copies, so only fds 0-2 survive exec.
  dup2(devnull, STDIN_FILENO);
  dup2(out_w, STDOUT_FILENO);
  dup2(err_w, STDERR_FILENO);
  execvp(spec->argv[0], spec->argv);
  dprintf(STDERR_FILENO, "logmux: cannot run %s: %s\n", spec->argv[0], strerror(errno));
  _exit(127);
}

typedef struct {
  int out[2];
  int err[2];
} child_pipes;

static void close_pipes(child_pipes* p) {
  for (int i = 0; i < 2; ++i) {
    if (p->out[i] >= 0) {
      close(p->out[i]);
    }
    if (p->err[i] >= 0) {
      close(p->err[i]);
    }
  }
}

static int open_pipes(child_pipes* p, bool cloexec) {
  *p = (child_pipes){{-1, -1}, {-1, -1}};
  const int ok_out = cloexec ? cloexec_pipe(p->out) : pipe(p->out);
  const int ok_err = ok_out == 0 ? (cloexec ? cloexec_pipe(p->err) : pipe(p->err)) : -1;
  if (ok_out != 0 || ok_err != 0) {
    close_pipes(p);
    return -1;
  }
  return 0;
}

// Forks and execs one child on the write ends of `p`. Signals stay blocked
// across fork so a handler can never run in the child before exec (it
// would write to logmux's wake pipe).
static pid_t fork_child(const lm_child_spec* spec, int devnull, const child_pipes* p) {
  sigset_t block;
  sigset_t previous;
  sigemptyset(&block);
  sigaddset(&block, SIGCHLD);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGTERM);
  sigprocmask(SIG_BLOCK, &block, &previous);
  const pid_t pid = fork();
  if (pid == 0) {
    child_exec(spec, devnull, p->out[1], p->err[1]);
  }
  sigprocmask(SIG_SETMASK, &previous, NULL);
  return pid;
}

// Parent side after a successful fork: the child's own process group, and
// non-blocking read ends.
static void adopt(child* c, pid_t pid, const child_pipes* p) {
  setpgid(pid, pid);  // also from the parent, so the group exists before any kill
  if (g_broken != LM_BROKEN_SEQUENTIAL_READS) {
    set_flags(p->out[0], 0, O_NONBLOCK);
    set_flags(p->err[0], 0, O_NONBLOCK);
  }
  c->result->pid = pid;
  c->result->spawned = true;
  c->streams[LM_STDOUT].fd = p->out[0];
  c->streams[LM_STDERR].fd = p->err[0];
}

// Each child's pipes are created just before its fork, close-on-exec, and
// the parent closes the write ends right after. Either measure alone keeps
// siblings from holding each other's write ends; together they also cover
// descriptors opened between spawns.
static bool spawn(child* c, int devnull) {
  child_pipes p;
  if (open_pipes(&p, true) != 0) {
    return false;
  }
  const pid_t pid = fork_child(c->spec, devnull, &p);
  close(p.out[1]);
  close(p.err[1]);
  if (pid < 0) {
    close(p.out[0]);
    close(p.err[0]);
    return false;
  }
  adopt(c, pid, &p);
  return true;
}

// LM_BROKEN_NAIVE_PIPES: every pipe first, inheritable, then every fork.
static void spawn_all_naively(child* children, size_t n, int devnull) {
  child_pipes* pipes = calloc(n == 0 ? 1 : n, sizeof *pipes);
  pid_t* pids = calloc(n == 0 ? 1 : n, sizeof *pids);
  for (size_t i = 0; i < n; ++i) {
    pipes[i] = (child_pipes){{-1, -1}, {-1, -1}};
    open_pipes(&pipes[i], false);
  }
  for (size_t i = 0; i < n; ++i) {
    pids[i] = pipes[i].out[0] >= 0 ? fork_child(children[i].spec, devnull, &pipes[i]) : -1;
  }
  for (size_t i = 0; i < n; ++i) {
    if (pipes[i].out[1] >= 0) {
      close(pipes[i].out[1]);
      close(pipes[i].err[1]);
    }
    if (pids[i] > 0) {
      adopt(&children[i], pids[i], &pipes[i]);
    }
  }
  free(pids);
  free(pipes);
}

static void signal_group(const child* c, int sig) {
  kill(-c->result->pid, sig);  // ESRCH once the whole group is gone is fine
}

static child* find_child(child* children, size_t n, pid_t pid) {
  for (size_t i = 0; i < n; ++i) {
    if (children[i].result->spawned && children[i].result->pid == pid) {
      return &children[i];
    }
  }
  return NULL;
}

static size_t reap(writer* w, child* children, size_t n) {
  size_t reaped = 0;
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    child* c = find_child(children, n, pid);
    if (c == NULL || c->reaped) {
      continue;
    }
    c->reaped = true;
    ++reaped;
    if (WIFEXITED(status)) {
      c->result->exited = true;
      c->result->exit_code = WEXITSTATUS(status);
      note(w, c, "exited with status %d", c->result->exit_code);
    } else if (WIFSIGNALED(status)) {
      c->result->signaled = true;
      c->result->term_signal = WTERMSIG(status);
      note(w, c, "killed by signal %d (%s)", c->result->term_signal,
           strsignal(c->result->term_signal));
    }
  }
  return reaped;
}

static bool has_open_stream(const child* c) {
  return c->streams[0].fd >= 0 || c->streams[1].fd >= 0;
}

static void close_stream(lm_poller* poller, stream* s) {
  lm_poller_remove(poller, s->fd);
  close(s->fd);
  s->fd = -1;
}

// --- The event loop -------------------------------------------------------------

static int remaining_ms(uint64_t deadline, uint64_t now) {
  if (deadline <= now) {
    return 0;
  }
  const uint64_t ms = (deadline - now + 999999u) / 1000000u;
  return ms > 60000 ? 60000 : (int)ms;
}

// Sends SIGTERM to children that have been silent too long, SIGKILL to
// those that ignored it, and returns the wait timeout until the next such
// deadline (-1 if there is none).
static int enforce_deadlines(writer* w, const lm_config* cfg, int grace_ms, child* children,
                             size_t n, bool shutting_down) {
  const uint64_t now = now_ns();
  int timeout = -1;
  for (size_t i = 0; i < n; ++i) {
    child* c = &children[i];
    if (!c->result->spawned || (c->reaped && !has_open_stream(c))) {
      continue;
    }
    if (c->kill_stage == 0) {
      const bool hung = cfg->hang_timeout_ms > 0 &&
                        now - c->last_output_ns >= (uint64_t)cfg->hang_timeout_ms * 1000000u;
      if (hung || shutting_down) {
        if (hung) {
          c->result->timed_out = true;
          note(w, c, "no output for %d ms; sending SIGTERM", cfg->hang_timeout_ms);
        } else {
          note(w, c, "shutting down; sending SIGTERM");
        }
        signal_group(c, SIGTERM);
        c->kill_stage = 1;
        c->term_sent_ns = now;
      } else if (cfg->hang_timeout_ms > 0) {
        const int t =
            remaining_ms(c->last_output_ns + (uint64_t)cfg->hang_timeout_ms * 1000000u, now);
        timeout = timeout < 0 || t < timeout ? t : timeout;
      }
    }
    if (c->kill_stage == 1) {
      const uint64_t deadline = c->term_sent_ns + (uint64_t)grace_ms * 1000000u;
      if (now >= deadline) {
        note(w, c, "still running %d ms after SIGTERM; sending SIGKILL", grace_ms);
        signal_group(c, SIGKILL);
        c->kill_stage = 2;
      } else {
        const int t = remaining_ms(deadline, now);
        timeout = timeout < 0 || t < timeout ? t : timeout;
      }
    }
  }
  return timeout;
}

static void drain_wake_pipe(int fd) {
  char buf[64];
  while (read(fd, buf, sizeof buf) > 0) {
  }
}

int lm_run(const lm_config* cfg, const lm_child_spec* specs, size_t n, lm_child_result* results,
           lm_stats* stats) {
  const size_t max_line = cfg->max_line == 0 ? DEFAULT_MAX_LINE : cfg->max_line;
  const int grace_ms = cfg->kill_grace_ms == 0 ? DEFAULT_GRACE_MS : cfg->kill_grace_ms;
  if (max_line > MAX_MAX_LINE || cfg->hang_timeout_ms < 0 || grace_ms < 0) {
    errno = EINVAL;
    return -1;
  }

  lm_stats local_stats;
  lm_stats* st = stats != NULL ? stats : &local_stats;
  memset(st, 0, sizeof *st);
  lm_child_result* res = results != NULL ? results : calloc(n == 0 ? 1 : n, sizeof *res);
  child* children = calloc(n == 0 ? 1 : n, sizeof *children);
  lm_poller* poller = lm_poller_create(cfg->backend);
  writer w = {.fd = cfg->out_fd, .cap = max_line + 512 > 65536 ? max_line + 512 : 65536};
  w.buf = malloc(w.cap);
  int wake[2] = {-1, -1};
  const int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
  int rc = -1;
  if (res == NULL || children == NULL || poller == NULL || w.buf == NULL || devnull < 0 ||
      cloexec_pipe(wake) != 0) {
    goto out;
  }
  set_flags(wake[0], 0, O_NONBLOCK);
  set_flags(wake[1], 0, O_NONBLOCK);
  if (lm_poller_add(poller, wake[0], NULL) != 0) {
    goto out;
  }

  memset(res, 0, n * sizeof *res);
  g_shutdown = 0;
  g_wake_fd = wake[1];
  saved_signals saved;
  install_signals(&saved);
  w.start_ns = now_ns();

  size_t living = 0;
  size_t open_streams = 0;
  for (size_t i = 0; i < n; ++i) {
    child* c = &children[i];
    c->spec = &specs[i];
    c->result = &res[i];
    for (int k = 0; k < 2; ++k) {
      c->streams[k] = (stream){.owner = c, .kind = k, .fd = -1};
    }
    c->last_output_ns = now_ns();
  }
  if (g_broken == LM_BROKEN_NAIVE_PIPES) {
    spawn_all_naively(children, n, devnull);
  }
  for (size_t i = 0; i < n; ++i) {
    child* c = &children[i];
    if (g_broken == LM_BROKEN_NAIVE_PIPES ? !c->result->spawned : !spawn(c, devnull)) {
      note(&w, c, "could not start: %s", strerror(errno));
      continue;
    }
    ++living;
    for (int k = 0; k < 2; ++k) {
      stream* s = &c->streams[k];
      s->buf = malloc(max_line + READ_CHUNK);
      if (s->buf == NULL || lm_poller_add(poller, s->fd, s) != 0) {
        // Out of memory, or a descriptor select cannot watch (FD_SETSIZE).
        note(&w, c, "cannot watch its %s pipe: %s", kStreamNames[k], strerror(errno));
        close(s->fd);
        s->fd = -1;
        continue;
      }
      ++open_streams;
    }
  }

  if (g_broken == LM_BROKEN_SEQUENTIAL_READS) {
    // Each stream to end-of-file in turn, blocking.
    for (size_t i = 0; i < n; ++i) {
      for (int k = 0; k < 2; ++k) {
        stream* s = &children[i].streams[k];
        while (s->fd >= 0) {
          const ssize_t got = read(s->fd, s->buf + s->len, max_line + READ_CHUNK - s->len);
          if (got > 0) {
            s->len += (size_t)got;
            frame_lines(&w, s, max_line);
            continue;
          }
          if (got < 0 && errno == EINTR) {
            continue;
          }
          flush_fragment(&w, s, max_line);
          close_stream(poller, s);
          --open_streams;
        }
      }
    }
    while (living > 0) {
      living -= reap(&w, children, n);
      if (living > 0) {
        const struct timespec ms = {0, 1000000};  // usleep is not in POSIX.1-2008
        nanosleep(&ms, NULL);
      }
    }
  }

  lm_event events[MAX_EVENTS];
  bool shutdown_sent = false;
  while (open_streams > 0 || living > 0) {
    living -= reap(&w, children, n);
    const bool shutting_down = g_shutdown || w.failed;
    const int timeout =
        enforce_deadlines(&w, cfg, grace_ms, children, n, shutting_down && !shutdown_sent);
    shutdown_sent = shutdown_sent || shutting_down;
    flush(&w);
    if (open_streams == 0 && living == 0) {
      break;
    }
    const int ready = lm_poller_wait(poller, timeout, events, MAX_EVENTS);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      goto restore;
    }
    if (ready > 0) {
      ++st->wakeups;
    }
    for (int e = 0; e < ready; ++e) {
      ++st->events;
      stream* s = events[e].tag;
      if (s == NULL) {
        drain_wake_pipe(wake[0]);
        continue;
      }
      // One read per ready stream per wakeup: a child flooding its pipe
      // cannot starve the others.
      const ssize_t got = read(s->fd, s->buf + s->len, max_line + READ_CHUNK - s->len);
      if (got > 0) {
        ++st->reads;
        st->bytes += (uint64_t)got;
        s->owner->result->bytes[s->kind] += (uint64_t)got;
        s->owner->last_output_ns = now_ns();
        s->len += (size_t)got;
        if (g_broken == LM_BROKEN_NO_REASSEMBLY) {
          // Whatever this read returned becomes a record, line or not.
          const size_t len = s->buf[s->len - 1] == '\n' ? s->len - 1 : s->len;
          record(&w, s, ':', s->buf, len);
          s->len = 0;
        } else {
          frame_lines(&w, s, max_line);
        }
      } else if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        flush_fragment(&w, s, max_line);
        close_stream(poller, s);
        --open_streams;
      }
    }
  }
  rc = 0;

restore:
  flush(&w);
  restore_signals(&saved);
  g_wake_fd = -1;
  st->seconds = (double)(now_ns() - w.start_ns) * 1e-9;
  st->records = w.records;
out:
  for (size_t i = 0; children != NULL && i < n; ++i) {
    for (int k = 0; k < 2; ++k) {
      if (children[i].streams[k].fd >= 0) {
        close_stream(poller, &children[i].streams[k]);
      }
      free(children[i].streams[k].buf);
    }
  }
  if (wake[0] >= 0) {
    close(wake[0]);
    close(wake[1]);
  }
  if (devnull >= 0) {
    close(devnull);
  }
  lm_poller_destroy(poller);
  free(w.buf);
  free(children);
  if (results == NULL) {
    free(res);
  }
  return rc;
}

int lm_run_broken(lm_broken kind, const lm_config* config, const lm_child_spec* children, size_t n,
                  lm_child_result* results, lm_stats* stats) {
  g_broken = (int)kind;
  const int rc = lm_run(config, children, n, results, stats);
  g_broken = -1;
  return rc;
}
