#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logmux/collector.h"
#include "logmux/poller.h"

#define MAX_CHILDREN 100000

static void print_usage(FILE* out) {
  fprintf(out,
          "usage: logmux [options] [-n N] -- command [args...]\n"
          "       logmux [options] -c NAME=COMMAND [-c NAME=COMMAND ...]\n"
          "\n"
          "Runs child processes and merges their stdout and stderr into one timestamped\n"
          "log, one record per line:\n"
          "\n"
          "  <seconds> <name> <stream><mark> <text>\n"
          "\n"
          "stream is out, err or sys (logmux's notes); mark is ':' for a complete line,\n"
          "'+' for a piece of an over-long line and '~' for a final line without a\n"
          "newline.\n"
          "\n"
          "  -n N               run N copies of the command, named <command>-1 ... -N\n"
          "  -c NAME=COMMAND    run COMMAND through /bin/sh -c, labelled NAME (repeatable)\n"
          "  --backend NAME     select | poll | epoll | kqueue (default: %s)\n"
          "  --max-line BYTES   cut longer lines into pieces (default: 4096)\n"
          "  --hang-timeout MS  SIGTERM a child silent this long (default: never)\n"
          "  --grace MS         SIGKILL this long after SIGTERM (default: 2000)\n"
          "  --output FILE      write the log to FILE instead of stdout\n"
          "  --stats            print per-child results and event-loop counters to stderr\n"
          "  --version, --help\n"
          "\n"
          "exit status: 0 every child exited 0, 1 otherwise, 2 usage or setup error\n",
          lm_backend_name(lm_backend_default()));
}

static void usage_error(const char* msg, const char* arg) {
  fprintf(stderr, "logmux: %s%s%s\n", msg, arg != NULL ? " " : "", arg != NULL ? arg : "");
  print_usage(stderr);
  exit(2);
}

static long parse_long(const char* flag, const char* text, long min, long max) {
  char* end = NULL;
  errno = 0;
  const long v = strtol(text, &end, 10);
  if (text[0] == '\0' || *end != '\0' || errno != 0 || v < min || v > max) {
    usage_error("bad value for", flag);
  }
  return v;
}

static void print_stats(const lm_child_spec* specs, const lm_child_result* r, size_t n,
                        const lm_stats* st, lm_backend backend) {
  fprintf(stderr, "%-16s %8s %-14s %10s %10s %9s\n", "child", "pid", "result", "out lines",
          "err lines", "fragments");
  for (size_t i = 0; i < n; ++i) {
    char result[32] = "not started";
    if (r[i].exited) {
      snprintf(result, sizeof result, "exit %d", r[i].exit_code);
    } else if (r[i].signaled) {
      snprintf(result, sizeof result, "signal %d%s", r[i].term_signal,
               r[i].timed_out ? " (hung)" : "");
    }
    fprintf(stderr, "%-16s %8d %-14s %10" PRIu64 " %10" PRIu64 " %9" PRIu64 "\n", specs[i].name,
            (int)r[i].pid, result, r[i].lines[LM_STDOUT], r[i].lines[LM_STDERR],
            r[i].fragments[LM_STDOUT] + r[i].fragments[LM_STDERR]);
  }
  fprintf(stderr,
          "backend %s: %.3f s (cpu %.3f s), %" PRIu64 " wakeups, %" PRIu64 " ready events, %" PRIu64
          " reads, %" PRIu64 " bytes, %" PRIu64 " records\n",
          lm_backend_name(backend), st->seconds, st->cpu_seconds, st->wakeups, st->events,
          st->reads, st->bytes, st->records);
}

int main(int argc, char** argv) {
  lm_config cfg = {.backend = lm_backend_default(), .out_fd = STDOUT_FILENO};
  long copies = 0;
  bool stats = false;
  const char* output = NULL;
  char** shell_commands = calloc((size_t)argc, sizeof *shell_commands);
  size_t shell_count = 0;
  int command_at = -1;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    const char* value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(arg, "--") == 0) {
      command_at = i + 1;
      break;
    }
    if (strcmp(arg, "--version") == 0) {
      printf("logmux %s\n", LOGMUX_VERSION);
      return 0;
    }
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout);
      return 0;
    }
    if (strcmp(arg, "--stats") == 0) {
      stats = true;
      continue;
    }
    if (value == NULL) {
      usage_error("missing value or unknown option:", arg);
    }
    if (strcmp(arg, "-n") == 0) {
      copies = parse_long(arg, value, 1, MAX_CHILDREN);
    } else if (strcmp(arg, "-c") == 0) {
      if (strchr(value, '=') == NULL || value[0] == '=') {
        usage_error("-c needs NAME=COMMAND, got", value);
      }
      shell_commands[shell_count++] = argv[i + 1];
    } else if (strcmp(arg, "--backend") == 0) {
      if (!lm_backend_parse(value, &cfg.backend)) {
        usage_error("unknown backend", value);
      }
      if (!lm_backend_available(cfg.backend)) {
        usage_error("backend not available on this platform:", value);
      }
    } else if (strcmp(arg, "--max-line") == 0) {
      cfg.max_line = (size_t)parse_long(arg, value, 1, 1 << 20);
    } else if (strcmp(arg, "--hang-timeout") == 0) {
      cfg.hang_timeout_ms = (int)parse_long(arg, value, 1, 86400000);
    } else if (strcmp(arg, "--grace") == 0) {
      cfg.kill_grace_ms = (int)parse_long(arg, value, 1, 86400000);
    } else if (strcmp(arg, "--output") == 0) {
      output = value;
    } else {
      usage_error("unknown option", arg);
    }
    ++i;
  }

  const bool have_command = command_at > 0 && command_at < argc;
  if (have_command == (shell_count > 0)) {
    usage_error(have_command ? "use either -c or a command after --, not both"
                             : "nothing to run: give -c NAME=COMMAND or -- command",
                NULL);
  }
  if (copies > 0 && !have_command) {
    usage_error("-n needs a command after --", NULL);
  }

  const size_t n = have_command ? (size_t)(copies > 0 ? copies : 1) : shell_count;
  lm_child_spec* specs = calloc(n, sizeof *specs);
  char** names = calloc(n, sizeof *names);
  char*** shell_argv = calloc(n, sizeof *shell_argv);
  lm_child_result* results = calloc(n, sizeof *results);
  if (specs == NULL || names == NULL || shell_argv == NULL || results == NULL) {
    fprintf(stderr, "logmux: out of memory\n");
    return 2;
  }
  for (size_t i = 0; i < n; ++i) {
    if (have_command) {
      const char* base = strrchr(argv[command_at], '/');
      base = base != NULL ? base + 1 : argv[command_at];
      names[i] = malloc(strlen(base) + 24);
      if (copies > 0) {
        sprintf(names[i], "%s-%zu", base, i + 1);
      } else {
        strcpy(names[i], base);
      }
      specs[i] = (lm_child_spec){names[i], argv + command_at};
    } else {
      char* eq = strchr(shell_commands[i], '=');
      names[i] = strndup(shell_commands[i], (size_t)(eq - shell_commands[i]));
      shell_argv[i] = calloc(4, sizeof(char*));
      shell_argv[i][0] = "/bin/sh";
      shell_argv[i][1] = "-c";
      shell_argv[i][2] = eq + 1;
      specs[i] = (lm_child_spec){names[i], shell_argv[i]};
    }
  }

  if (output != NULL) {
    cfg.out_fd = open(output, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (cfg.out_fd < 0) {
      fprintf(stderr, "logmux: cannot open %s: %s\n", output, strerror(errno));
      return 2;
    }
  }

  lm_stats st;
  if (lm_run(&cfg, specs, n, results, &st) != 0) {
    fprintf(stderr, "logmux: %s\n", strerror(errno));
    return 2;
  }
  if (stats) {
    print_stats(specs, results, n, &st, cfg.backend);
  }
  int status = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!results[i].exited || results[i].exit_code != 0) {
      status = 1;
    }
  }
  return status;
}
