#!/usr/bin/env python3
"""Benchmarks the logmux event-loop backends and reports the results.

    scripts/experiments.py run --build build-rel --label macos [--note TEXT]
    scripts/experiments.py report

`run` benchmarks every backend this machine has and writes
results/<label>/runs.json, with the machine description. Run it once per
platform: natively on macOS (select, poll, kqueue) and on Linux (select,
poll, epoll). `report` reads every results/*/runs.json and writes
results/summary.md and the charts. Backends are only ever compared within
one platform.

Two experiments:
  idle   logmux-bench: K busy pipes carrying 1 GiB plus N idle pipes, for N
         from 0 to 4000. Measures what watching idle descriptors costs.
  e2e    the logmux collector itself with 16-256 children writing as fast
         as they can, output to /dev/null.

Every point is the median of several runs; the spread is kept. Only the
Python standard library is used.
"""

import argparse
import json
import math
import os
import platform
import resource
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
IDLE = [0, 64, 256, 500, 1000, 2000, 4000]
BUSY = 4
MB = 1024
REPEAT = 5
E2E_CHILDREN = [16, 64, 256]
E2E_TOTAL_LINES = 2_000_000
E2E_REPEAT = 3


def sh(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def machine(note):
    info = {"system": platform.system(), "kernel": platform.release()}
    if platform.system() == "Darwin":
        info.update({
            "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
            "performance_cores": sh(["sysctl", "-n", "hw.perflevel0.physicalcpu"]),
            "efficiency_cores": sh(["sysctl", "-n", "hw.perflevel1.physicalcpu"]),
            "memory_gb": round(int(sh(["sysctl", "-n", "hw.memsize"])) / 2**30),
            "os": "macOS " + sh(["sw_vers", "-productVersion"]),
        })
    else:
        info.update({"cpus_visible": os.cpu_count(),
                     "os": sh(["sh", "-c", ". /etc/os-release && echo $PRETTY_NAME"]),
                     "in_container": os.path.exists("/.dockerenv")})
    info["compiler"] = sh(["cc", "--version"]).splitlines()[0]
    if note:
        info["note"] = note
    return info


def available_backends(build):
    names = []
    for b in ["select", "poll", "epoll", "kqueue"]:
        proc = subprocess.run([os.path.join(build, "logmux-bench"), "--backend", b, "--idle", "0",
                               "--mb", "1"], capture_output=True, text=True)
        if proc.returncode == 0:
            names.append(b)
    return names


def run_idle(build, backends):
    runs = []
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "r.json")
        for backend in backends:
            for idle in IDLE:
                for rep in range(REPEAT):
                    print(f"  idle: {backend} N={idle} run {rep + 1}", file=sys.stderr, flush=True)
                    subprocess.run([os.path.join(build, "logmux-bench"), "--backend", backend,
                                    "--idle", str(idle), "--busy", str(BUSY), "--mb", str(MB),
                                    "--json", out], check=True, capture_output=True)
                    r = json.load(open(out))
                    r["repeat"] = rep
                    runs.append(r)
                    if not r["supported"]:
                        break  # select past FD_SETSIZE: no point repeating
    return runs


def run_e2e(build, backends):
    runs = []
    with tempfile.TemporaryDirectory() as tmp:
        stats_file = os.path.join(tmp, "stats.txt")
        for backend in backends:
            for n in E2E_CHILDREN:
                lines = E2E_TOTAL_LINES // n
                for rep in range(E2E_REPEAT):
                    print(f"  e2e: {backend} children={n} run {rep + 1}", file=sys.stderr,
                          flush=True)
                    cmd = [os.path.join(build, "logmux"), "--backend", backend, "--stats",
                           "--output", "/dev/null", "-n", str(n), "--",
                           os.path.join(build, "logmux-child"), "--id", "1", "--lines",
                           str(lines), "--mode", "flood", "--max-len", "200"]
                    with open(stats_file, "w") as err:
                        proc = subprocess.run(cmd, stderr=err)
                    text = open(stats_file).read()
                    if proc.returncode != 0:
                        sys.exit(f"logmux failed:\n{' '.join(cmd)}\n{text}")
                    last = text.strip().splitlines()[-1]
                    # backend kqueue: 0.379 s (cpu 0.041 s), 20 wakeups, ... N records
                    seconds = float(last.split(": ")[1].split(" s")[0])
                    cpu = float(last.split("(cpu ")[1].split(" s)")[0])
                    records = int(last.rsplit(", ", 1)[1].split()[0])
                    runs.append({"backend": backend, "children": n, "repeat": rep,
                                 "seconds": seconds, "cpu_seconds": cpu, "records": records,
                                 "lines_expected": lines * n})
    return runs


def cmd_run(args):
    build = os.path.abspath(args.build)
    out_dir = os.path.join(RESULTS, args.label)
    os.makedirs(out_dir, exist_ok=True)
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    backends = available_backends(build)
    print(f"backends here: {', '.join(backends)}", file=sys.stderr)
    data = {"machine": machine(args.note), "backends": backends,
            "parameters": {"idle": IDLE, "busy": BUSY, "mb": MB, "repeat": REPEAT,
                           "e2e_children": E2E_CHILDREN, "e2e_total_lines": E2E_TOTAL_LINES,
                           "e2e_repeat": E2E_REPEAT, "fd_limit": [soft, hard]},
            "idle": run_idle(build, backends),
            "e2e": run_e2e(build, backends)}
    json.dump(data, open(os.path.join(out_dir, "runs.json"), "w"), indent=1)
    print(f"wrote {os.path.join(out_dir, 'runs.json')}", file=sys.stderr)


# --------------------------------------------------------------------------
# Report

def med(values):
    return statistics.median(values) if values else float("nan")


def idle_points(runs):
    """(backend, idle) -> medians, or None if unsupported."""
    out = {}
    for r in runs:
        key = (r["backend"], r["idle"])
        out.setdefault(key, []).append(r)
    points = {}
    for key, rs in out.items():
        if not rs[0]["supported"]:
            points[key] = None
            continue
        mib = [r["mib_per_second"] for r in rs]
        points[key] = {
            "mib": med(mib), "mib_min": min(mib), "mib_max": max(mib),
            "cpu_us_wakeup": med([r["cpu_us_per_wakeup"] for r in rs]),
            "cpu_per_gib": med([r["cpu_seconds"] / (r["mb"] / 1024) for r in rs]),
            "ping_p50_us": med([r["ping_p50_ns"] / 1000 for r in rs]),
            "ping_p99_us": med([r["ping_p99_ns"] / 1000 for r in rs]),
        }
    return points


def table(header, rows):
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|"]
    out += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(out)


def cell(p, fmt):
    return "n/a (FD_SETSIZE)" if p is None else fmt(p)


def platform_section(label, data):
    m = data["machine"]
    pts = idle_points(data["idle"])
    backends = data["backends"]
    lines = [f"## {label}", ""]
    lines += [f"- {k.replace('_', ' ')}: {v}" for k, v in m.items()]
    lines += ["", f"### Idle descriptors: {BUSY} busy pipes carrying {MB} MiB, plus N idle pipes",
              "", "Median of 5 runs. Throughput in MiB/s with [min-max].", ""]
    lines += [table(["Backend"] + [f"N={n}" for n in IDLE],
                    [[f"`{b}`"] + [cell(pts.get((b, n)), lambda p: f"{p['mib']:.0f} "
                                        f"[{p['mib_min']:.0f}-{p['mib_max']:.0f}]")
                                   for n in IDLE] for b in backends]), ""]
    lines += ["Reader CPU per wakeup (µs):", ""]
    lines += [table(["Backend"] + [f"N={n}" for n in IDLE],
                    [[f"`{b}`"] + [cell(pts.get((b, n)), lambda p: f"{p['cpu_us_wakeup']:.1f}")
                                   for n in IDLE] for b in backends]), ""]
    lines += ["Ping latency, write to read, p50 / p99 (µs):", ""]
    lines += [table(["Backend"] + [f"N={n}" for n in IDLE],
                    [[f"`{b}`"] + [cell(pts.get((b, n)),
                                        lambda p: f"{p['ping_p50_us']:.0f} / {p['ping_p99_us']:.0f}")
                                   for n in IDLE] for b in backends]), ""]

    e2e = {}
    for r in data["e2e"]:
        e2e.setdefault((r["backend"], r["children"]), []).append(r)
    lines += [f"### End to end: logmux collecting {E2E_TOTAL_LINES:,} lines from N children "
              "writing as fast as they can", "",
              "Median of 3 runs: wall seconds / logmux CPU seconds.", ""]
    lines += [table(["Backend"] + [f"{n} children" for n in E2E_CHILDREN],
                    [[f"`{b}`"] + [f"{med([r['seconds'] for r in e2e[(b, n)]]):.2f} / "
                                   f"{med([r['cpu_seconds'] for r in e2e[(b, n)]]):.2f}"
                                   for n in E2E_CHILDREN] for b in backends]), ""]
    return "\n".join(lines), pts


# --- Charts (plain SVG, light and dark) -------------------------------------

LIGHT = {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
         "grid": "#e1e0d9", "axis": "#c3c2b7", "s1": "#2a78d6", "s2": "#eb6834", "s3": "#1baf7a"}
DARK = {"surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
        "grid": "#2c2c2a", "axis": "#383835", "s1": "#3987e5", "s2": "#d95926", "s3": "#199e70"}
FONT = 'system-ui, -apple-system, "Segoe UI", sans-serif'
CLASS = {"select": "s2", "poll": "s3", "epoll": "s1", "kqueue": "s1"}


def style():
    def rules(p):
        fills = "".join(f".{k}{{fill:{v}}}" for k, v in p.items())
        strokes = "".join(f".l{k}{{stroke:{p[k]}}}" for k in ("s1", "s2", "s3"))
        return (fills + strokes + f".gridline{{stroke:{p['grid']}}}"
                f".baseline{{stroke:{p['axis']}}}.ring{{stroke:{p['surface']}}}")
    return (f"<style>text{{font-family:{FONT}}}{rules(LIGHT)}"
            f"@media (prefers-color-scheme: dark){{{rules(DARK)}}}</style>")


def line_chart(title, subtitle, series, *, y_label, log, fmt):
    """series: [(name, class, {idle: value or None})]; idle counts on a
    categorical axis (they span three orders of magnitude)."""
    width, height, left, right, top, bottom = 760, 396, 72, 150, 100, 56
    pw, ph = width - left - right, height - top - bottom
    values = [v for _, _, ys in series for v in ys.values() if v is not None]
    if log:
        lo = 10 ** math.floor(math.log10(min(values)))
        hi = 10 ** math.ceil(math.log10(max(values)))
        ticks = [lo * 10 ** i for i in range(round(math.log10(hi / lo)) + 1)]
        sy = lambda v: top + ph - (math.log10(v) - math.log10(lo)) / math.log10(hi / lo) * ph
    else:
        raw = max(values) / 5
        mag = 10 ** math.floor(math.log10(raw))
        step = next(m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw)
        ticks = [i * step for i in range(math.ceil(max(values) / step - 1e-9) + 1)]
        sy = lambda v: top + ph - v / ticks[-1] * ph
    sx = lambda i: left + i * pw / (len(IDLE) - 1)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
           f'width="{width}" height="{height}" role="img" aria-label="{title}">', style(),
           f'<rect class="surface" width="{width}" height="{height}" rx="8"/>',
           f'<text class="ink" x="16" y="26" font-size="15" font-weight="600">{title}</text>',
           f'<text class="ink2" x="16" y="45" font-size="12">{subtitle}</text>']
    x = 16
    for name, cls, _ in series:
        out.append(f'<rect class="{cls}" x="{x}" y="56" width="10" height="10" rx="2"/>')
        out.append(f'<text class="ink2" x="{x + 15}" y="65" font-size="12">{name}</text>')
        x += 15 + 7 * len(name) + 24
    for t in ticks:
        y = sy(t)
        out.append(f'<line class="gridline" x1="{left}" x2="{left + pw}" y1="{y:.1f}" y2="{y:.1f}" '
                   'stroke-width="1"/>')
        out.append(f'<text class="muted" x="{left - 8}" y="{y + 4:.1f}" font-size="11" '
                   f'text-anchor="end">{fmt(t)}</text>')
    for i, n in enumerate(IDLE):
        out.append(f'<text class="muted" x="{sx(i):.1f}" y="{top + ph + 18}" font-size="11" '
                   f'text-anchor="middle">{n}</text>')
    out.append(f'<text class="muted" x="{left + pw / 2:.1f}" y="{height - 14}" font-size="11" '
               'text-anchor="middle">idle pipes watched (steps not to scale)</text>')
    out.append(f'<text class="muted" x="16" y="{top - 16}" font-size="11">{y_label}</text>')
    out.append(f'<line class="baseline" x1="{left}" x2="{left + pw}" y1="{top + ph}" '
               f'y2="{top + ph}" stroke-width="1"/>')
    for name, cls, ys in series:
        pts = [(sx(i), sy(ys[n])) for i, n in enumerate(IDLE) if ys.get(n) is not None]
        path = " ".join(f"{'M' if i == 0 else 'L'}{px:.1f},{py:.1f}" for i, (px, py) in enumerate(pts))
        out.append(f'<path class="l{cls}" d="{path}" fill="none" stroke-width="2" '
                   'stroke-linejoin="round" stroke-linecap="round"/>')
        for (px, py), n in zip(pts, [n for n in IDLE if ys.get(n) is not None]):
            out.append(f'<circle class="{cls} ring" cx="{px:.1f}" cy="{py:.1f}" r="4" '
                       f'stroke-width="2"><title>{name}, {n} idle: {fmt(ys[n])}</title></circle>')
        px, py = pts[-1]
        last_n = [n for n in IDLE if ys.get(n) is not None][-1]
        label = name if last_n == IDLE[-1] else f"{name} (stops at {last_n}: FD_SETSIZE)"
        out.append(f'<text class="ink2" x="{px + 10:.1f}" y="{py + 4:.1f}" font-size="11">'
                   f'{label}</text>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def cmd_report(_args):
    labels = sorted(d for d in os.listdir(RESULTS)
                    if os.path.exists(os.path.join(RESULTS, d, "runs.json")))
    if not labels:
        sys.exit("no results/*/runs.json yet: run `experiments.py run` first")
    sections = ["# Results", "",
                "Generated by `scripts/experiments.py report`. Backends are compared only "
                "within one platform.", ""]
    for label in labels:
        data = json.load(open(os.path.join(RESULTS, label, "runs.json")))
        text, pts = platform_section(label, data)
        sections.append(text)
        fast = [b for b in data["backends"] if b in ("epoll", "kqueue")]
        series = [(b, CLASS[b], {n: (pts[(b, n)] or {}).get("cpu_us_wakeup") for n in IDLE})
                  for b in data["backends"]]
        open(os.path.join(RESULTS, f"cpu_per_wakeup_{label}.svg"), "w").write(line_chart(
            f"Cost of watching idle pipes ({label})",
            f"Reader CPU per wakeup, {BUSY} busy pipes plus N idle ones, log scale",
            series, y_label="CPU per wakeup", log=True,
            fmt=lambda v: f"{v:g} µs"))
        series = [(b, CLASS[b], {n: (pts[(b, n)] or {}).get("mib") for n in IDLE})
                  for b in data["backends"]]
        open(os.path.join(RESULTS, f"throughput_{label}.svg"), "w").write(line_chart(
            f"Throughput with idle pipes ({label})",
            f"MiB/s drained from {BUSY} busy pipes while N idle pipes are watched",
            series, y_label="MiB/s", log=False, fmt=lambda v: f"{v:,.0f}"))
        if fast:
            f = fast[0]
            for other in ("select", "poll"):
                key = (other, 1000) if (other, 1000) in pts and pts[(other, 1000)] else None
                if key and pts[(f, 1000)]:
                    sections.append(f"- {label}, 1000 idle pipes: `{f}` drains "
                                    f"{pts[(f, 1000)]['mib'] / pts[key]['mib']:.1f}x the "
                                    f"throughput of `{other}` with "
                                    f"{pts[key]['cpu_us_wakeup'] / pts[(f, 1000)]['cpu_us_wakeup']:.1f}x "
                                    "less CPU per wakeup.")
            sections.append("")
    open(os.path.join(RESULTS, "summary.md"), "w").write("\n".join(sections))
    print("wrote results/summary.md and charts", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="benchmark this machine's backends")
    run.add_argument("--build", default=os.path.join(ROOT, "build-rel"))
    run.add_argument("--label", required=True, help="results/<label>/, e.g. macos or linux")
    run.add_argument("--note", default="", help="free text stored with the machine details")
    sub.add_parser("report", help="summarize every results/*/runs.json")
    args = parser.parse_args()
    return cmd_run(args) if args.command == "run" else cmd_report(args)


if __name__ == "__main__":
    sys.exit(main())
