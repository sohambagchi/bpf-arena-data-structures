#!/usr/bin/env python3
"""
Benchmarking script for BPF arena data structures.

Runs executables with varying batch sizes and collects throughput
(messages/sec) and tail latency (p50, p99) over multiple runs.

Setup
-----
One producer, one consumer, fixed-size 64-byte messages, varying batch
sizes from 1 to 256.  Both BPF-relay (skeleton_*) and pure-userspace
(usertest_*) executables are supported.  The producer enqueues N messages;
the consumer dequeues and acknowledges.  We report median throughput
(messages per second) and tail latency (p50, p99) over configurable
runs (default 10).

Output
------
A formatted results table plus optional CSV.  The parseable metrics
emitted by ds_metrics_dump_parseable() (BENCH_METRICS_BEGIN / BENCH /
BENCH_METRICS_END sentinels) are consumed when present; otherwise the
legacy ds_metrics_print table is parsed as a fallback.  Wall-clock
throughput is always computed externally as well.
"""

from __future__ import annotations

import argparse
import csv
import io
import multiprocessing
import os
import re
import signal
import statistics
import subprocess
import sys
import threading
import time
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

TRACE_PIPE_PATH = "/sys/kernel/debug/tracing/trace_pipe"
TRACE_CLEAR_PATH = "/sys/kernel/debug/tracing/trace"

DEFAULT_BATCH_SIZES = [1, 2, 4, 8, 16, 32, 64, 128, 256]
DEFAULT_RUNS = 10
DEFAULT_DURATION_SEC = 10

# Regex patterns for the machine-parseable metrics output
# (emitted by ds_metrics_dump_parseable in ds_metrics.h)
BENCH_BEGIN_RE = re.compile(r"^BENCH_METRICS_BEGIN\s+(.+)$")
BENCH_LINE_RE = re.compile(
    r"^BENCH\s+(\S+)\s+"
    r"total=(\d+)\s+success=(\d+)\s+avg_ns=(\d+)\s+"
    r"p50_ns=(\d+)\s+p99_ns=(\d+)\s+tput=(\d+)"
)
BENCH_ELAPSED_RE = re.compile(r"^BENCH_ELAPSED_SEC\s+([\d.]+)")
BENCH_END_RE = re.compile(r"^BENCH_METRICS_END")

# Legacy table parser (for executables that only call ds_metrics_print)
LEGACY_METRICS_RE = re.compile(
    r"^(\S[\w ]+?)\s{2,}"  # category name (left-aligned)
    r"(\d+)\s+"  # Total
    r"(\d+)\s+"  # Success
    r"[\d.]+%\s+"  # Rate%
    r"(\d+)\s+"  # Avg(ns)
    r"(\d+)\s+"  # Avg-OK(ns)
    r"(\d+)\s*$"  # Tput-OK
)


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


@dataclass
class CategoryMetrics:
    """Parsed metrics for one category in one run."""

    tag: str
    total: int = 0
    success: int = 0
    avg_ns: int = 0
    p50_ns: int = 0
    p99_ns: int = 0
    throughput: int = 0


@dataclass
class TrialResult:
    """Result of a single benchmark trial (one executable, one batch size)."""

    executable: str
    batch_size: int
    run_idx: int
    elapsed_sec: float = 0.0
    return_code: int = -1
    categories: Dict[str, CategoryMetrics] = field(default_factory=dict)
    wallclock_throughput: float = 0.0  # success_ops / elapsed
    ds_name: str = ""


@dataclass
class AggregatedResult:
    """Statistics aggregated over multiple runs for one (exe, batch_size)."""

    executable: str
    batch_size: int
    num_runs: int = 0
    median_throughput: float = 0.0
    median_p50_ns: float = 0.0
    median_p99_ns: float = 0.0
    all_throughputs: List[float] = field(default_factory=list)
    all_p50: List[float] = field(default_factory=list)
    all_p99: List[float] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Executable discovery
# ---------------------------------------------------------------------------

# Ordered list of known BPF-relay executables
SKELETON_CANDIDATES = [
    "skeleton_msqueue",
    "skeleton_vyukhov",
    "skeleton_folly_spsc",
    "skeleton_ck_fifo_spsc",
    "skeleton_ck_ring_spsc",
    "skeleton_ck_stack_upmc",
    "skeleton_io_uring",
    "skeleton_kcov",
]

USERTEST_CANDIDATES = [
    "usertest_msqueue",
    "usertest_vyukhov",
    "usertest_folly_spsc",
    "usertest_ck_fifo_spsc",
    "usertest_ck_ring_spsc",
    "usertest_ck_stack_upmc",
]


def find_executables(
    build_dir: str = "build",
    include_usertest: bool = False,
) -> List[str]:
    """Return paths to available benchmark executables under *build_dir*.

    By default only BPF skeleton programs are returned.  Pass
    *include_usertest=True* (or use ``--usertest-only`` / ``--include-usertest``)
    to also include pure-userspace test binaries.
    """
    candidates = list(SKELETON_CANDIDATES)
    if include_usertest:
        candidates += USERTEST_CANDIDATES

    found: List[str] = []
    for name in candidates:
        path = os.path.join(build_dir, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            found.append(path)
    return found


def is_skeleton(exe: str) -> bool:
    return "skeleton_" in os.path.basename(exe)


# ---------------------------------------------------------------------------
# Trace-pipe helpers (for BPF skeleton programs)
# ---------------------------------------------------------------------------


def clear_trace_buffer() -> None:
    try:
        with open(TRACE_CLEAR_PATH, "w", encoding="utf-8") as f:
            f.write("")
    except Exception:
        pass


def trace_pipe_reader(output_path: Path, stop_event: threading.Event) -> None:
    """Background thread: copy trace_pipe to a file until stopped."""
    proc = None
    with output_path.open("w", encoding="utf-8") as out:
        try:
            proc = subprocess.Popen(
                ["sudo", "cat", TRACE_PIPE_PATH],
                stdout=out,
                stderr=subprocess.DEVNULL,
                text=True,
            )
            while not stop_event.is_set():
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
        except Exception:
            pass
        finally:
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


# ---------------------------------------------------------------------------
# Workload generator (batch-aware touch workers)
# ---------------------------------------------------------------------------


def batch_touch_worker(
    worker_id: int,
    core_id: int,
    batch_size: int,
    ready_event,
    start_event,
    stop_event,
    touch_pids,
) -> None:
    """
    Create files in bursts of *batch_size* to trigger inode_create hooks.

    Each burst rapidly touches *batch_size* distinct files (one fork+exec
    each), then sleeps briefly before the next burst.  This lets the
    downstream relay process the batch before the next burst arrives.
    """
    try:
        os.sched_setaffinity(0, {core_id})
    except Exception:
        pass

    ready_event.set()
    start_event.wait()

    while not stop_event.is_set():
        # --- produce one batch ---
        for i in range(batch_size):
            fname = f"bench_{worker_id}_{i}.tmp"
            try:
                p = subprocess.Popen(["touch", fname])
                touch_pids.append(p.pid)
                p.wait()
            except Exception:
                pass

        # Brief inter-batch gap so the consumer can drain
        time.sleep(0.001 * batch_size)

        # --- cleanup created files ---
        for i in range(batch_size):
            fname = f"bench_{worker_id}_{i}.tmp"
            try:
                if os.path.exists(fname):
                    os.remove(fname)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Output parsing
# ---------------------------------------------------------------------------


def parse_parseable_metrics(text: str) -> Optional[Dict[str, CategoryMetrics]]:
    """
    Parse BENCH_METRICS_BEGIN / BENCH ... / BENCH_METRICS_END blocks.

    Returns a dict mapping category tag -> CategoryMetrics, or None if no
    parseable block was found.
    """
    cats: Dict[str, CategoryMetrics] = {}
    inside = False

    for line in text.splitlines():
        line = line.strip()

        if BENCH_BEGIN_RE.match(line):
            inside = True
            continue

        if BENCH_END_RE.match(line):
            break

        if not inside:
            continue

        m = BENCH_LINE_RE.match(line)
        if m:
            tag = m.group(1)
            cats[tag] = CategoryMetrics(
                tag=tag,
                total=int(m.group(2)),
                success=int(m.group(3)),
                avg_ns=int(m.group(4)),
                p50_ns=int(m.group(5)),
                p99_ns=int(m.group(6)),
                throughput=int(m.group(7)),
            )
            continue

    return cats if cats else None


def parse_bench_elapsed(text: str) -> float:
    """Extract BENCH_ELAPSED_SEC from output, or return 0."""
    for line in text.splitlines():
        m = BENCH_ELAPSED_RE.match(line.strip())
        if m:
            return float(m.group(1))
    return 0.0


def parse_legacy_metrics(text: str) -> Optional[Dict[str, CategoryMetrics]]:
    """
    Fallback parser for the human-readable ds_metrics_print table.

    Maps known category names to tags.  Returns None if nothing parsed.
    """
    name_to_tag = {
        "LKMM producer": "lkmm_producer",
        "User consumer": "user_consumer",
        "User producer": "user_producer",
        "LKMM consumer": "lkmm_consumer",
        "End-to-end": "end_to_end",
    }
    cats: Dict[str, CategoryMetrics] = {}

    for line in text.splitlines():
        m = LEGACY_METRICS_RE.match(line.strip())
        if not m:
            continue
        name = m.group(1).strip()
        tag = name_to_tag.get(name)
        if not tag:
            continue
        cats[tag] = CategoryMetrics(
            tag=tag,
            total=int(m.group(2)),
            success=int(m.group(3)),
            avg_ns=int(m.group(4)),
            p50_ns=0,  # not available in legacy format
            p99_ns=0,
            throughput=int(m.group(6)),
        )

    return cats if cats else None


def parse_done_line(text: str) -> Optional[int]:
    """Parse 'done: produced=N consumed=N' from usertest output."""
    m = re.search(r"done: produced=(\d+) consumed=(\d+)", text)
    if m:
        return int(m.group(2))
    return None


def parse_relay_count(text: str) -> Optional[int]:
    """Parse 'KU popped=N' from skeleton statistics output.

    All skeleton programs print this line in their print_statistics()
    function.  This is the ground-truth count of messages successfully
    relayed through the kernel->user->kernel pipeline and is a more
    reliable throughput numerator than the metrics ring success count
    (which can be inflated by busy-loop empty-queue polls).
    """
    m = re.search(r"KU popped=(\d+)", text)
    if m:
        return int(m.group(1))
    return None


# ---------------------------------------------------------------------------
# Trial runner
# ---------------------------------------------------------------------------


def run_trial(
    executable: str,
    batch_size: int,
    run_idx: int,
    duration: int,
    show_output: bool = False,
    build_dir: str = "build",
) -> TrialResult:
    """
    Run a single benchmark trial for *executable* at *batch_size*.

    For skeleton (BPF) executables the trial replicates the runner.py
    pattern: spawn touch workers in batches, capture trace_pipe, terminate
    after *duration* seconds.

    For usertest executables the trial sets the DS_BENCH_BATCH_SIZE
    environment variable and runs the program to completion (or timeout).
    """
    result = TrialResult(
        executable=executable,
        batch_size=batch_size,
        run_idx=run_idx,
    )
    env = os.environ.copy()
    env["DS_BENCH_BATCH_SIZE"] = str(batch_size)

    if is_skeleton(executable):
        result = _run_skeleton_trial(
            executable,
            batch_size,
            run_idx,
            duration,
            show_output,
            build_dir,
            env,
            result,
        )
    else:
        result = _run_usertest_trial(
            executable,
            batch_size,
            run_idx,
            duration,
            show_output,
            env,
            result,
        )

    return result


def _run_skeleton_trial(
    executable: str,
    batch_size: int,
    run_idx: int,
    duration: int,
    show_output: bool,
    build_dir: str,
    env: dict,
    result: TrialResult,
) -> TrialResult:
    """BPF skeleton benchmark: touch workers + relay + trace_pipe."""

    nworkers = 1
    manager = multiprocessing.Manager()
    start_event = manager.Event()
    stop_event = manager.Event()
    ready_events = [manager.Event() for _ in range(nworkers)]
    touch_pids = manager.list()

    workers = []
    for w in range(nworkers):
        p = multiprocessing.Process(
            target=batch_touch_worker,
            args=(
                w,
                w % multiprocessing.cpu_count(),
                batch_size,
                ready_events[w],
                start_event,
                stop_event,
                touch_pids,
            ),
        )
        p.start()
        workers.append(p)

    for ev in ready_events:
        ev.wait()

    # Trace pipe capture
    clear_trace_buffer()
    trace_log = Path(build_dir) / (
        f"{Path(executable).name}_bench_b{batch_size}_r{run_idx}.log"
    )
    trace_stop = threading.Event()
    trace_thr = threading.Thread(
        target=trace_pipe_reader,
        args=(trace_log, trace_stop),
        daemon=True,
    )
    trace_thr.start()

    # Launch the BPF executable
    t0 = time.monotonic()
    exe_proc = subprocess.Popen(
        [f"./{executable}"],
        stdout=subprocess.PIPE if not show_output else None,
        stderr=subprocess.PIPE if not show_output else None,
        text=True,
        env=env,
    )

    # Signal workers to begin producing
    start_event.set()
    time.sleep(duration)

    # Terminate — give the skeleton time to join relay thread,
    # trigger kernel consumer uprobe loop, and print statistics.
    # The uprobe loop can iterate uk_enqueued_count + 1024 times,
    # so 15 seconds is more appropriate than the original 5.
    exe_proc.terminate()
    try:
        stdout, stderr = exe_proc.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        exe_proc.kill()
        stdout, stderr = exe_proc.communicate()
    t1 = time.monotonic()

    stop_event.set()
    for w in workers:
        w.join(timeout=3)
    trace_stop.set()
    trace_thr.join(timeout=3)

    # Cleanup temp files
    for w_id in range(nworkers):
        for i in range(batch_size):
            fname = f"bench_{w_id}_{i}.tmp"
            try:
                os.remove(fname)
            except OSError:
                pass

    result.elapsed_sec = t1 - t0
    result.return_code = exe_proc.returncode

    if stdout:
        _populate_result_from_output(result, stdout)

    return result


def _run_usertest_trial(
    executable: str,
    batch_size: int,
    run_idx: int,
    timeout: int,
    show_output: bool,
    env: dict,
    result: TrialResult,
) -> TrialResult:
    """Pure userspace benchmark trial."""

    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            [f"./{executable}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=env,
            timeout=timeout + 30,
        )
        stdout = proc.stdout or ""
        result.return_code = proc.returncode
    except subprocess.TimeoutExpired:
        stdout = ""
        result.return_code = -1
    t1 = time.monotonic()

    result.elapsed_sec = t1 - t0

    if stdout:
        if show_output:
            sys.stdout.write(stdout)
        _populate_result_from_output(result, stdout)

        # For usertests, derive wallclock throughput from consumed count
        consumed = parse_done_line(stdout)
        if consumed and result.elapsed_sec > 0:
            result.wallclock_throughput = consumed / result.elapsed_sec

    return result


def _populate_result_from_output(result: TrialResult, stdout: str) -> None:
    """Fill result.categories from parseable or legacy metrics in stdout."""
    # Try parseable format first
    cats = parse_parseable_metrics(stdout)
    if cats is None:
        cats = parse_legacy_metrics(stdout)

    if cats:
        result.categories = cats

    # Check for embedded elapsed override
    bench_elapsed = parse_bench_elapsed(stdout)
    if bench_elapsed > 0:
        result.elapsed_sec = bench_elapsed

    # --- Throughput computation ---
    # For skeleton programs, the ground-truth relay count ("KU popped=N")
    # is far more reliable than the metrics ring success count.  The
    # latter can be inflated by busy-loop empty-queue polls recorded
    # as metric samples (the success flag is per-sample, but the
    # aggregate success_count in the ring counter reflects actual
    # DS_SUCCESS returns, which in some data structures can include
    # vacuous successes).  The relay count is always exactly the number
    # of messages that traversed the full pipeline.
    relay_count = parse_relay_count(stdout)
    if relay_count is not None and relay_count > 0 and result.elapsed_sec > 0:
        result.wallclock_throughput = relay_count / result.elapsed_sec
        return

    # Fallback: derive from the best available metrics category
    if cats:
        for pref in (
            "user_consumer",
            "lkmm_producer",
            "user_producer",
            "lkmm_consumer",
        ):
            if pref in cats and cats[pref].success > 0:
                # Use the internally-computed throughput from the metrics
                # (success / success_latency) when available, as it is
                # more robust than success / wallclock.
                if cats[pref].throughput > 0:
                    tput = float(cats[pref].throughput)
                elif result.elapsed_sec > 0:
                    tput = cats[pref].success / result.elapsed_sec
                else:
                    tput = 0.0

                # Sanity cap: no single-machine throughput should exceed
                # 10 billion msg/s (well beyond any realistic BPF relay).
                MAX_SANE_THROUGHPUT = 10_000_000_000.0
                if tput > MAX_SANE_THROUGHPUT:
                    tput = 0.0  # treat as invalid

                result.wallclock_throughput = tput
                break


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------


def aggregate_trials(
    trials: List[TrialResult],
) -> AggregatedResult:
    """
    Compute median throughput and latency percentiles over trials.

    Throughput: median of wallclock_throughput across runs.
    p50 / p99:  median of the per-run p50/p99 values reported by the
                best-available metrics category.
    """
    if not trials:
        return AggregatedResult(executable="", batch_size=0)

    exe = trials[0].executable
    bs = trials[0].batch_size

    throughputs: List[float] = []
    p50s: List[float] = []
    p99s: List[float] = []

    for t in trials:
        throughputs.append(t.wallclock_throughput)

        # Pick the best category for latency (prefer end_to_end for true
        # pipeline latency; fall back to per-category if unavailable)
        best = None
        for pref in (
            "end_to_end",
            "user_consumer",
            "lkmm_producer",
            "user_producer",
            "lkmm_consumer",
        ):
            if pref in t.categories and t.categories[pref].success > 0:
                best = t.categories[pref]
                break

        if best:
            p50s.append(float(best.p50_ns))
            p99s.append(float(best.p99_ns))

    # Filter out zero throughputs (failed runs)
    valid_tput = [v for v in throughputs if v > 0]

    agg = AggregatedResult(
        executable=exe,
        batch_size=bs,
        num_runs=len(trials),
        median_throughput=statistics.median(valid_tput) if valid_tput else 0.0,
        median_p50_ns=statistics.median(p50s) if p50s else 0.0,
        median_p99_ns=statistics.median(p99s) if p99s else 0.0,
        all_throughputs=throughputs,
        all_p50=p50s,
        all_p99=p99s,
    )
    return agg


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------


def print_results_table(
    results: Dict[str, List[AggregatedResult]],
) -> None:
    """Print a formatted summary table to stdout."""

    hdr = (
        f"{'Executable':<32s} {'Batch':>5s} {'Runs':>4s} "
        f"{'Med Tput (msg/s)':>18s} {'Med p50 (us)':>14s} "
        f"{'Med p99 (us)':>14s}"
    )
    sep = "=" * len(hdr)

    print(f"\n{sep}")
    print("              BENCHMARK RESULTS")
    print(sep)
    print(hdr)
    print("-" * len(hdr))

    for exe_name in sorted(results.keys()):
        for agg in results[exe_name]:
            p50_us = agg.median_p50_ns / 1000.0 if agg.median_p50_ns else 0.0
            p99_us = agg.median_p99_ns / 1000.0 if agg.median_p99_ns else 0.0
            print(
                f"{os.path.basename(agg.executable):<32s} "
                f"{agg.batch_size:>5d} "
                f"{agg.num_runs:>4d} "
                f"{agg.median_throughput:>18,.0f} "
                f"{p50_us:>14.2f} "
                f"{p99_us:>14.2f}"
            )

    print(sep)
    print()


def write_csv(
    results: Dict[str, List[AggregatedResult]],
    path: str,
) -> None:
    """Write results as CSV."""
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "executable",
                "batch_size",
                "num_runs",
                "median_throughput_msg_per_sec",
                "median_p50_ns",
                "median_p99_ns",
            ]
        )
        for exe_name in sorted(results.keys()):
            for agg in results[exe_name]:
                writer.writerow(
                    [
                        os.path.basename(agg.executable),
                        agg.batch_size,
                        agg.num_runs,
                        f"{agg.median_throughput:.2f}",
                        f"{agg.median_p50_ns:.0f}",
                        f"{agg.median_p99_ns:.0f}",
                    ]
                )
    print(f"CSV written to {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def parse_batch_sizes(s: str) -> List[int]:
    """Parse a comma-separated list of batch sizes, with optional ranges."""
    sizes: List[int] = []
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        sizes.append(int(part))
    return sorted(set(sizes))


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark BPF arena data structures with varying batch sizes.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "By default only BPF skeleton programs are benchmarked.\n"
            "Use --include-usertest or --usertest-only to add/select\n"
            "pure-userspace test executables.\n"
            "\n"
            "Examples:\n"
            "  # Benchmark all skeleton executables, default settings\n"
            "  sudo python3 scripts/benchmarking.py\n"
            "\n"
            "  # Benchmark only skeleton_msqueue, batch sizes 1 and 64, 5 runs\n"
            "  sudo python3 scripts/benchmarking.py skeleton_msqueue "
            "--batch-sizes 1,64 --runs 5\n"
            "\n"
            "  # Include usertests too, write CSV\n"
            "  sudo python3 scripts/benchmarking.py --include-usertest "
            "--output bench_results.csv\n"
            "\n"
            "  # Benchmark usertests only\n"
            "  python3 scripts/benchmarking.py --usertest-only\n"
        ),
    )
    parser.add_argument(
        "executables",
        nargs="*",
        help=(
            "Executable names to benchmark (e.g. skeleton_msqueue or "
            "build/skeleton_msqueue).  Default: all found in build/."
        ),
    )
    parser.add_argument(
        "--batch-sizes",
        "-b",
        type=str,
        default=",".join(str(s) for s in DEFAULT_BATCH_SIZES),
        help=(
            "Comma-separated list of batch sizes to test "
            f"(default: {','.join(str(s) for s in DEFAULT_BATCH_SIZES)})"
        ),
    )
    parser.add_argument(
        "--runs",
        "-r",
        type=int,
        default=DEFAULT_RUNS,
        help=f"Number of runs per (executable, batch_size) pair (default: {DEFAULT_RUNS})",
    )
    parser.add_argument(
        "--duration",
        "-d",
        type=int,
        default=DEFAULT_DURATION_SEC,
        help=f"Duration in seconds per trial for skeleton executables (default: {DEFAULT_DURATION_SEC})",
    )
    parser.add_argument(
        "--output",
        "-o",
        type=str,
        default=None,
        help="Write results to a CSV file",
    )
    parser.add_argument(
        "--show-output",
        action="store_true",
        help="Stream executable stdout/stderr to the terminal",
    )
    parser.add_argument(
        "--include-usertest",
        action="store_true",
        help="Also include pure-userspace test executables (default: skeleton only)",
    )
    parser.add_argument(
        "--usertest-only",
        action="store_true",
        help="Benchmark only pure-userspace test executables",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Verbose progress output",
    )

    args = parser.parse_args(argv)

    batch_sizes = parse_batch_sizes(args.batch_sizes)
    if not batch_sizes:
        print("Error: no valid batch sizes specified", file=sys.stderr)
        return 1

    # ---- discover executables ----
    include_usertest = args.include_usertest or args.usertest_only
    all_exes = find_executables(include_usertest=include_usertest)

    if args.usertest_only:
        all_exes = [e for e in all_exes if not is_skeleton(e)]

    if args.executables:
        filter_set = set(args.executables)
        all_exes = [
            e for e in all_exes if e in filter_set or os.path.basename(e) in filter_set
        ]

    if not all_exes:
        print("No executables found in build/. Run `make` first.", file=sys.stderr)
        return 1

    # ---- banner ----
    print("=" * 72)
    print("  BPF Arena Data Structures - Benchmark Suite")
    print("=" * 72)
    print(f"  Executables : {', '.join(os.path.basename(e) for e in all_exes)}")
    print(f"  Batch sizes : {batch_sizes}")
    print(f"  Runs/config : {args.runs}")
    print(f"  Duration(s) : {args.duration} (skeleton programs)")
    print(f"  CPU count   : {multiprocessing.cpu_count()}")
    print("=" * 72)
    print()

    # ---- run benchmark matrix ----
    # results[exe_basename] -> list of AggregatedResult (one per batch size)
    results: Dict[str, List[AggregatedResult]] = defaultdict(list)

    total_trials = len(all_exes) * len(batch_sizes) * args.runs
    trial_num = 0

    for exe in all_exes:
        exe_base = os.path.basename(exe)
        for bs in batch_sizes:
            trials: List[TrialResult] = []
            for run_idx in range(args.runs):
                trial_num += 1
                if args.verbose or trial_num % 5 == 1:
                    print(
                        f"  [{trial_num}/{total_trials}] "
                        f"{exe_base}  batch={bs}  run={run_idx + 1}/{args.runs}",
                        flush=True,
                    )

                tr = run_trial(
                    executable=exe,
                    batch_size=bs,
                    run_idx=run_idx,
                    duration=args.duration,
                    show_output=args.show_output,
                )
                trials.append(tr)

                if args.verbose:
                    tput = tr.wallclock_throughput
                    print(
                        f"         -> rc={tr.return_code}  "
                        f"elapsed={tr.elapsed_sec:.2f}s  "
                        f"tput={tput:,.0f} msg/s"
                    )

            agg = aggregate_trials(trials)
            results[exe_base].append(agg)

            print(
                f"  {exe_base}  batch={bs:>3d}  "
                f"median_tput={agg.median_throughput:>12,.0f} msg/s  "
                f"p50={agg.median_p50_ns / 1000:.1f}us  "
                f"p99={agg.median_p99_ns / 1000:.1f}us"
            )

    # ---- report ----
    print_results_table(results)

    if args.output:
        write_csv(results, args.output)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
