# logmux

Runs many child processes and merges their stdout and stderr into one timestamped log,
without blocking on any child and without losing or tearing lines. The event loop has
interchangeable backends (`select`, `poll`, `epoll` on Linux, `kqueue` on macOS), so
their costs can be measured against each other.

> Work in progress. Design, results and usage will be documented here as the
> milestones land.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

| Option | Default | Effect |
|---|---|---|
| `LOGMUX_WERROR` | `OFF` | Treat warnings as errors (on in CI) |
| `LOGMUX_SANITIZE` | `OFF` | Build with ASan and UBSan |
| `LOGMUX_BUILD_TESTS` | `ON` | Build the tests |

## License

MIT, see [LICENSE](LICENSE).
