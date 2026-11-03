// logmux-bench: the cost of the event loop itself, per backend.
//
//   logmux-bench --backend B --idle N [--busy K] [--mb M] [--ping-us P]
//                [--json FILE]
//
// Watches N idle pipes (open, never written) and K busy pipes, each fed by a
// writer process sending M/K MiB in 4 KiB writes as fast as it can, plus one
// pinger that writes a timestamp every P microseconds. The reader drains
// everything with lm_poller and reports:
//
//   - wall time and throughput until every busy pipe reaches end-of-file
//   - the reader's own CPU time (user + system), and per wakeup
//   - wakeups and ready events
//   - ping latency: from the pinger's write to the reader seeing it
//
// Idle pipes cost no processes, so N can reach thousands. Registration
// happens before the clock starts; the measurement is waiting and reading.
// select cannot watch descriptors at or above FD_SETSIZE, and reports that
// instead of a result.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "histogram.h"
#include "logmux/poller.h"
#include "timer.h"

#define WRITE_SIZE 4096
#define READ_SIZE 65536
#define MAX_EVENTS 256

typedef struct {
  lm_backend backend;
  long idle;
  long busy;
  double mb;
  long ping_us;
  const char* json;
} options;

typedef struct {
  int fd;
  int kind;  // 0 idle, 1 busy, 2 ping
} source;

static void usage(void) {
  fprintf(stderr,
          "usage: logmux-bench --backend B --idle N [--busy K] [--mb M] [--ping-us P] "
          "[--json FILE]\n");
  exit(2);
}

static double cpu_seconds(void) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6 +
         (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;
}

static void raise_fd_limit(long needed) {
  struct rlimit lim;
  getrlimit(RLIMIT_NOFILE, &lim);
  if ((long)lim.rlim_cur >= needed) {
    return;
  }
  lim.rlim_cur = (rlim_t)needed;
  if (lim.rlim_max != RLIM_INFINITY && lim.rlim_cur > lim.rlim_max) {
    lim.rlim_cur = lim.rlim_max;
  }
  if (setrlimit(RLIMIT_NOFILE, &lim) != 0) {
    fprintf(stderr, "logmux-bench: cannot raise the descriptor limit to %ld: %s\n", needed,
            strerror(errno));
    exit(2);
  }
}

// A writer process: `bytes` to fd in WRITE_SIZE writes, then exit.
static pid_t start_writer(int fd, long long bytes) {
  const pid_t pid = fork();
  if (pid == 0) {
    char buf[WRITE_SIZE];
    memset(buf, 'x', sizeof buf);
    while (bytes > 0) {
      const size_t n = bytes < WRITE_SIZE ? (size_t)bytes : WRITE_SIZE;
      if (write(fd, buf, n) < 0) {
        _exit(1);
      }
      bytes -= (long long)n;
    }
    _exit(0);
  }
  return pid;
}

// The pinger: a timestamp every `interval_us` until killed. Each message is
// 16 bytes, so reads never split one.
static pid_t start_pinger(int fd, long interval_us) {
  const pid_t pid = fork();
  if (pid == 0) {
    for (;;) {
      uint64_t msg[2] = {sl_now_ns(), 0};
      if (write(fd, msg, sizeof msg) < 0) {
        _exit(0);
      }
      const struct timespec ts = {interval_us / 1000000, (interval_us % 1000000) * 1000};
      nanosleep(&ts, NULL);
    }
  }
  return pid;
}

int main(int argc, char** argv) {
  options o = {.backend = lm_backend_default(), .busy = 4, .mb = 256, .ping_us = 500, .idle = -1};
  for (int i = 1; i + 1 < argc; i += 2) {
    const char* flag = argv[i];
    const char* value = argv[i + 1];
    if (strcmp(flag, "--backend") == 0) {
      if (!lm_backend_parse(value, &o.backend) || !lm_backend_available(o.backend)) {
        fprintf(stderr, "logmux-bench: backend %s is not available here\n", value);
        return 2;
      }
    } else if (strcmp(flag, "--idle") == 0) {
      o.idle = strtol(value, NULL, 10);
    } else if (strcmp(flag, "--busy") == 0) {
      o.busy = strtol(value, NULL, 10);
    } else if (strcmp(flag, "--mb") == 0) {
      o.mb = strtod(value, NULL);
    } else if (strcmp(flag, "--ping-us") == 0) {
      o.ping_us = strtol(value, NULL, 10);
    } else if (strcmp(flag, "--json") == 0) {
      o.json = value;
    } else {
      usage();
    }
  }
  if (o.idle < 0 || o.busy < 1 || o.mb <= 0 || o.ping_us < 1 || (argc - 1) % 2 != 0) {
    usage();
  }
  signal(SIGPIPE, SIG_IGN);
  raise_fd_limit(2 * (o.idle + o.busy + 1) + 64);

  const long total = o.idle + o.busy + 1;
  source* src = calloc((size_t)total, sizeof *src);
  int* write_ends = calloc((size_t)total, sizeof *write_ends);
  lm_poller* poller = lm_poller_create(o.backend);
  bool supported = true;
  for (long i = 0; i < total; ++i) {
    int fds[2];
    if (pipe(fds) != 0) {
      fprintf(stderr, "logmux-bench: pipe: %s\n", strerror(errno));
      return 2;
    }
    // Busy pipes and the pinger are created first: on Linux, once a user's
    // pipes hold pipe-user-pages-soft pages (64 MiB by default), new pipes
    // get 8 KiB buffers instead of 64 KiB, which would throttle the busy
    // pipes whenever many idle ones exist.
    src[i] = (source){fds[0], i < o.busy ? 1 : i == o.busy ? 2 : 0};
    write_ends[i] = fds[1];
    if (lm_poller_add(poller, fds[0], &src[i]) != 0) {
      supported = false;  // select, past FD_SETSIZE
    }
  }
  if (!supported) {
    printf("%s: cannot watch %ld pipes (descriptors reach FD_SETSIZE = %d)\n",
           lm_backend_name(o.backend), total, FD_SETSIZE);
    if (o.json != NULL) {
      FILE* f = fopen(o.json, "w");
      fprintf(f, "{\"backend\": \"%s\", \"idle\": %ld, \"busy\": %ld, \"supported\": false}\n",
              lm_backend_name(o.backend), o.idle, o.busy);
      fclose(f);
    }
    lm_poller_destroy(poller);
    free(write_ends);
    free(src);
    return 0;
  }

  // Writers start before the clock; their pipes fill and they wait.
  const long long per_writer = (long long)(o.mb * 1048576.0 / (double)o.busy);
  pid_t* writers = calloc((size_t)o.busy, sizeof *writers);
  for (long k = 0; k < o.busy; ++k) {
    writers[k] = start_writer(write_ends[k], per_writer);
    close(write_ends[k]);  // the reader sees EOF when the writer exits
    write_ends[k] = -1;
  }
  const pid_t pinger = start_pinger(write_ends[o.busy], o.ping_us);

  sl_histogram latency;
  sl_hist_clear(&latency);
  char* buf = malloc(READ_SIZE);
  lm_event events[MAX_EVENTS];
  long busy_open = o.busy;
  unsigned long long wakeups = 0;
  unsigned long long ready_events = 0;
  unsigned long long bytes = 0;
  const double cpu0 = cpu_seconds();
  const uint64_t t0 = sl_now_ns();
  while (busy_open > 0) {
    const int n = lm_poller_wait(poller, -1, events, MAX_EVENTS);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "logmux-bench: wait: %s\n", strerror(errno));
      return 2;
    }
    ++wakeups;
    ready_events += (unsigned long long)n;
    for (int e = 0; e < n; ++e) {
      source* s = events[e].tag;
      const ssize_t got = read(s->fd, buf, READ_SIZE);
      if (s->kind == 2) {
        const uint64_t now = sl_now_ns();
        for (ssize_t off = 0; off + 16 <= got; off += 16) {
          uint64_t sent;
          memcpy(&sent, buf + off, sizeof sent);
          sl_hist_record(&latency, now - sent);
        }
      } else if (got > 0) {
        bytes += (unsigned long long)got;
      } else if (got == 0) {
        lm_poller_remove(poller, s->fd);
        close(s->fd);
        --busy_open;
      }
    }
  }
  const double wall = (double)(sl_now_ns() - t0) * 1e-9;
  const double cpu = cpu_seconds() - cpu0;

  kill(pinger, SIGKILL);
  waitpid(pinger, NULL, 0);
  for (long k = 0; k < o.busy; ++k) {
    waitpid(writers[k], NULL, 0);
  }

  const double mbps = (double)bytes / 1048576.0 / wall;
  const uint64_t p50 = sl_hist_quantile(&latency, 0.50);
  const uint64_t p99 = sl_hist_quantile(&latency, 0.99);
  printf(
      "%-6s idle %5ld busy %2ld: %8.1f MiB/s, cpu %.3f s (%.2f us/wakeup), %llu wakeups, "
      "%llu events, ping p50 %.1f us p99 %.1f us\n",
      lm_backend_name(o.backend), o.idle, o.busy, mbps, cpu, cpu * 1e6 / (double)wakeups, wakeups,
      ready_events, (double)p50 / 1000, (double)p99 / 1000);
  if (o.json != NULL) {
    FILE* f = fopen(o.json, "w");
    if (f == NULL) {
      fprintf(stderr, "logmux-bench: cannot write %s\n", o.json);
      return 2;
    }
    fprintf(f,
            "{\"backend\": \"%s\", \"idle\": %ld, \"busy\": %ld, \"supported\": true, "
            "\"mb\": %.1f, \"seconds\": %.6f, \"mib_per_second\": %.2f, \"cpu_seconds\": %.6f, "
            "\"cpu_us_per_wakeup\": %.3f, \"wakeups\": %llu, \"events\": %llu, "
            "\"ping_samples\": %llu, \"ping_p50_ns\": %llu, \"ping_p99_ns\": %llu, "
            "\"clock_resolution_ns\": %.2f}\n",
            lm_backend_name(o.backend), o.idle, o.busy, (double)bytes / 1048576.0, wall, mbps, cpu,
            cpu * 1e6 / (double)wakeups, wakeups, ready_events, (unsigned long long)latency.total,
            (unsigned long long)p50, (unsigned long long)p99, sl_timer_resolution_ns());
    fclose(f);
  }
  for (long i = 0; i < total; ++i) {
    if (src[i].kind != 1) {  // busy read ends were closed at end-of-file
      close(src[i].fd);
    }
    if (write_ends[i] >= 0) {
      close(write_ends[i]);
    }
  }
  lm_poller_destroy(poller);
  free(buf);
  free(writers);
  free(write_ends);
  free(src);
  return 0;
}
