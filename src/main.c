#include <stdio.h>
#include <string.h>

static void print_usage(FILE* out) {
  fprintf(out,
          "usage: logmux [--version] [--help]\n"
          "\n"
          "Runs child processes and merges their output into one timestamped log.\n");
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--version") == 0) {
      printf("logmux %s\n", LOGMUX_VERSION);
      return 0;
    }
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(stdout);
      return 0;
    }
    fprintf(stderr, "logmux: unknown argument '%s'\n", argv[i]);
    print_usage(stderr);
    return 2;
  }
  print_usage(stderr);
  return 2;
}
