// logmux-child: a test child that writes predictable lines in hostile ways,
// so the verifier can check that logmux collected exactly what was written.
//
//   logmux-child --id N --lines L [--mode MODE] [--seed S] [--max-len B]
//                [--chunk C] [--delay-us D]
//
// Lines come from payload.h. Output is cut into write() calls of random
// sizes up to --chunk bytes, regardless of line boundaries, so a line often
// spans several reads on the collector side.
//
// Modes:
//   normal       L lines to stdout, exit 0
//   both         each line to stdout or stderr (chosen by payload.h)
//   flood        L lines to stdout in writes of up to 64 KiB, no delay
//   trickle      L lines to stdout one byte per write, --delay-us apart
//   crash        L lines, then the first half of line L, then abort()
//   no-newline   L lines, the last one without its newline
//   early-close  L/2 lines to stdout, close stdout, L lines to stderr
//   hang         L lines to stdout, then sleep until killed
//   stall-stderr 128 KiB to stderr first, then L lines to stdout (deadlocks a
//                collector that reads stdout to the end before stderr)

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "payload.h"

typedef struct {
  unsigned id;
  unsigned long long lines;
  unsigned long long seed;
  size_t max_len;
  size_t chunk;
  long delay_us;
  const char* mode;
  uint64_t rng;
} options;

static void usage(void) {
  fprintf(stderr,
          "usage: logmux-child --id N --lines L [--mode MODE] [--seed S] [--max-len B]\n"
          "                    [--chunk C] [--delay-us D]\n");
  exit(2);
}

static void pause_us(long us) {
  if (us > 0) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
  }
}

// Writes all of buf to fd in pieces of random size up to opt->chunk.
static void write_chunked(options* opt, int fd, const char* buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    opt->rng = lm_mix(opt->rng);
    size_t piece = 1 + (size_t)(opt->rng % opt->chunk);
    if (piece > len - off) {
      piece = len - off;
    }
    const ssize_t n = write(fd, buf + off, piece);
    if (n <= 0) {
      exit(3);  // the collector went away
    }
    off += (size_t)n;
    pause_us(opt->delay_us);
  }
}

static void write_line(options* opt, int fd, char* line, unsigned long long seq, int newline) {
  size_t len = lm_make_line(line, opt->seed, opt->id, seq, opt->max_len);
  if (newline) {
    line[len++] = '\n';
  }
  write_chunked(opt, fd, line, len);
}

int main(int argc, char** argv) {
  options opt = {.max_len = 512, .chunk = 512, .mode = "normal"};
  int have_id = 0;
  int have_lines = 0;
  for (int i = 1; i + 1 < argc; i += 2) {
    const char* flag = argv[i];
    const char* value = argv[i + 1];
    if (strcmp(flag, "--id") == 0) {
      opt.id = (unsigned)strtoul(value, NULL, 10);
      have_id = 1;
    } else if (strcmp(flag, "--lines") == 0) {
      opt.lines = strtoull(value, NULL, 10);
      have_lines = 1;
    } else if (strcmp(flag, "--seed") == 0) {
      opt.seed = strtoull(value, NULL, 10);
    } else if (strcmp(flag, "--max-len") == 0) {
      opt.max_len = (size_t)strtoull(value, NULL, 10);
    } else if (strcmp(flag, "--chunk") == 0) {
      opt.chunk = (size_t)strtoull(value, NULL, 10);
    } else if (strcmp(flag, "--delay-us") == 0) {
      opt.delay_us = strtol(value, NULL, 10);
    } else if (strcmp(flag, "--mode") == 0) {
      opt.mode = value;
    } else {
      usage();
    }
  }
  if (!have_id || !have_lines || opt.chunk == 0 || (argc - 1) % 2 != 0) {
    usage();
  }
  opt.rng = lm_mix(opt.seed ^ opt.id);
  char* line = malloc(lm_line_capacity(opt.max_len) + 1);
  const char* m = opt.mode;

  if (strcmp(m, "flood") == 0) {
    opt.chunk = 65536;
  } else if (strcmp(m, "trickle") == 0) {
    opt.chunk = 1;
  }

  if (strcmp(m, "normal") == 0 || strcmp(m, "flood") == 0 || strcmp(m, "trickle") == 0 ||
      strcmp(m, "hang") == 0) {
    // Lines go out through one buffer so flood writes span many lines.
    size_t cap = 1 << 16;
    char* buf = malloc(cap + lm_line_capacity(opt.max_len) + 1);
    size_t used = 0;
    for (unsigned long long s = 0; s < opt.lines; ++s) {
      size_t len = lm_make_line(buf + used, opt.seed, opt.id, s, opt.max_len);
      buf[used + len] = '\n';
      used += len + 1;
      if (used >= cap) {
        write_chunked(&opt, STDOUT_FILENO, buf, used);
        used = 0;
      }
    }
    write_chunked(&opt, STDOUT_FILENO, buf, used);
    free(buf);
    if (strcmp(m, "hang") == 0) {
      for (;;) {
        pause();
      }
    }
  } else if (strcmp(m, "both") == 0) {
    for (unsigned long long s = 0; s < opt.lines; ++s) {
      const int fd = lm_line_stream(opt.seed, opt.id, s) == 0 ? STDOUT_FILENO : STDERR_FILENO;
      write_line(&opt, fd, line, s, 1);
    }
  } else if (strcmp(m, "crash") == 0) {
    for (unsigned long long s = 0; s < opt.lines; ++s) {
      write_line(&opt, STDOUT_FILENO, line, s, 1);
    }
    const size_t len = lm_make_line(line, opt.seed, opt.id, opt.lines, opt.max_len);
    write_chunked(&opt, STDOUT_FILENO, line, len / 2);
    abort();
  } else if (strcmp(m, "no-newline") == 0) {
    for (unsigned long long s = 0; s < opt.lines; ++s) {
      write_line(&opt, STDOUT_FILENO, line, s, s + 1 < opt.lines);
    }
  } else if (strcmp(m, "early-close") == 0) {
    for (unsigned long long s = 0; s < opt.lines / 2; ++s) {
      write_line(&opt, STDOUT_FILENO, line, s, 1);
    }
    close(STDOUT_FILENO);
    for (unsigned long long s = 0; s < opt.lines; ++s) {
      write_line(&opt, STDERR_FILENO, line, s, 1);
    }
  } else if (strcmp(m, "stall-stderr") == 0) {
    // 128 KiB to stderr fills its pipe (64 KiB on Linux and macOS) unless
    // the collector reads stderr while stdout is still open.
    for (unsigned long long s = 0; s < 4096; ++s) {
      const size_t len = (size_t)snprintf(line, 64, "err filler %llu %031d\n", s, 0);
      write_chunked(&opt, STDERR_FILENO, line, len);
    }
    for (unsigned long long s = 0; s < opt.lines; ++s) {
      write_line(&opt, STDOUT_FILENO, line, s, 1);
    }
  } else {
    fprintf(stderr, "logmux-child: unknown mode %s\n", m);
    return 2;
  }
  free(line);
  return 0;
}
