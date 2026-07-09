"""
Runner script for BPF arena relay executables.
Spawns concurrent processes on separate cores to stress test during execution.

Prints a DATA_STRUCTURES_1.md-style comparison table at the end of every run.
This runner remains relay-oriented; for one-way arena-vs-ringbuf comparisons,
use `scripts/benchmarking.py --one-way`.

With --final N, repeats all data structure runs N times (default 1000),
writes a timestamped CSV with per-run metrics, and generates a CDF graph
of E2E latency per data structure.
"""

import os
import subprocess
import multiprocessing
import time
import argparse
import csv
import threading
import re
import statistics
import sys
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional


TRACE_PIPE_PATH = "/sys/kernel/debug/tracing/trace_pipe"
TRACE_CLEAR_PATH = "/sys/kernel/debug/tracing/trace"
TRACE_CONSUME_RE = re.compile(r"consume key=(\d+) value=(\d+)")

# ---------------------------------------------------------------------------
# Metrics parsing (matches BENCH parseable output from ds_metrics.h)
# ---------------------------------------------------------------------------

BENCH_BEGIN_RE = re.compile(r"^BENCH_METRICS_BEGIN\s+(.+)$")
BENCH_LINE_RE = re.compile(
    r"^BENCH\s+(\S+)\s+"
    r"total=(\d+)\s+success=(\d+)\s+avg_ns=(\d+)\s+"
    r"p50_ns=(\d+)\s+p99_ns=(\d+)\s+tput=(\d+)"
)
BENCH_END_RE = re.compile(r"^BENCH_METRICS_END")

# Legacy table parser (for executables that only call ds_metrics_print
# without the parseable block — unlikely now, but kept as fallback)
LEGACY_METRICS_RE = re.compile(
    r"^(\S[\w ]+?)\s{2,}"  # category name (left-aligned)
    r"(\d+)\s+"  # Total
    r"(\d+)\s+"  # Success
    r"[\d.]+%\s+"  # Rate%
    r"(\d+)\s+"  # Avg(ns)
    r"(\d+)\s+"  # Avg-OK(ns)
    r"(\d+)\s*$"  # Tput-OK
)


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
class RunMetrics:
    """Parsed metrics from one data structure run."""

    ds_name: str = ""
    categories: Dict[str, CategoryMetrics] = field(default_factory=dict)
    ku_popped: int = 0
    uk_pushed: int = 0

    @property
    def lkmm_prod_ns(self) -> int:
        c = self.categories.get("lkmm_producer")
        return c.avg_ns if c else 0

    @property
    def user_cons_ns(self) -> int:
        c = self.categories.get("user_consumer")
        return c.avg_ns if c else 0

    @property
    def user_prod_ns(self) -> int:
        c = self.categories.get("user_producer")
        return c.avg_ns if c else 0

    @property
    def lkmm_cons_ns(self) -> int:
        c = self.categories.get("lkmm_consumer")
        return c.avg_ns if c else 0

    @property
    def e2e_ns(self) -> int:
        return (
            self.lkmm_prod_ns
            + self.user_cons_ns
            + self.user_prod_ns
            + self.lkmm_cons_ns
        )

    @property
    def tput_mops(self) -> float:
        e = self.e2e_ns
        return (1e9 / e) / 1e6 if e > 0 else 0.0

    @property
    def relay_pct(self) -> float:
        if self.ku_popped > 0:
            return self.uk_pushed / self.ku_popped * 100.0
        return 0.0


def parse_parseable_metrics(text: str) -> Optional[Dict[str, CategoryMetrics]]:
    """Parse BENCH_METRICS_BEGIN / BENCH ... / BENCH_METRICS_END blocks."""
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

    return cats if cats else None


def parse_legacy_metrics(text: str) -> Optional[Dict[str, CategoryMetrics]]:
    """Fallback parser for the human-readable ds_metrics_print table."""
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
            avg_ns=int(m.group(4)),  # Avg(ns) column — but Avg-OK(ns) is group(5)
            p50_ns=0,
            p99_ns=0,
            throughput=int(m.group(6)),
        )

    # Fix: for legacy, the parseable format emits avg_ns = Avg-OK(ns),
    # but the legacy table has Avg(ns) in group(4) and Avg-OK(ns) in group(5).
    # We want Avg-OK(ns) to match the parseable format.
    for line in text.splitlines():
        m = LEGACY_METRICS_RE.match(line.strip())
        if not m:
            continue
        name = m.group(1).strip()
        tag = name_to_tag.get(name)
        if tag and tag in cats:
            cats[tag].avg_ns = int(m.group(5))  # Avg-OK(ns)

    return cats if cats else None


def parse_relay_counts(text: str) -> tuple:
    """Parse 'KU popped=N UK pushed=N' from skeleton output.
    Returns (ku_popped, uk_pushed) or (0, 0) if not found."""
    m = re.search(r"KU popped=(\d+)\s+UK pushed=(\d+)", text)
    if m:
        return int(m.group(1)), int(m.group(2))
    return 0, 0


def parse_ds_name(text: str) -> str:
    """Extract DS name from BENCH_METRICS_BEGIN or PERFORMANCE METRICS header."""
    m = BENCH_BEGIN_RE.search(text)
    if m:
        return m.group(1).strip()
    m = re.search(r"PERFORMANCE METRICS:\s+(.+)", text)
    if m:
        return m.group(1).strip()
    return ""


def parse_run_output(text: str) -> Optional[RunMetrics]:
    """Parse the full stdout of a single DS run into RunMetrics."""
    if not text:
        return None

    cats = parse_parseable_metrics(text)
    if cats is None:
        cats = parse_legacy_metrics(text)

    if not cats:
        return None

    ku_popped, uk_pushed = parse_relay_counts(text)
    ds_name = parse_ds_name(text)

    metrics = RunMetrics(
        ds_name=ds_name,
        categories=cats,
        ku_popped=ku_popped,
        uk_pushed=uk_pushed,
    )
    return metrics


# ---------------------------------------------------------------------------
# DS name mapping (executable basename -> display name for table)
# ---------------------------------------------------------------------------

EXE_TO_DS_NAME = {
    "skeleton_ck_fifo_spsc": "CK FIFO SPSC",
    "skeleton_folly_spsc": "Folly SPSC",
    "skeleton_ck_ring_spsc": "CK Ring SPSC",
    "skeleton_kcov": "KCOV Buffer",
    "skeleton_io_uring": "IO_URING Ring",
    "skeleton_iouring_liburing": "IOURING_LIBURING Ring",
    "skeleton_vyukhov": "Vyukhov MPMC",
    "skeleton_ck_stack_upmc": "CK Stack UPMC",
    "skeleton_msqueue": "MSQueue",
}

EXE_TO_DS_TYPE = {
    "skeleton_ck_fifo_spsc": "queue",
    "skeleton_folly_spsc": "queue",
    "skeleton_ck_ring_spsc": "ring",
    "skeleton_kcov": "buffer",
    "skeleton_io_uring": "ring",
    "skeleton_iouring_liburing": "ring",
    "skeleton_vyukhov": "queue",
    "skeleton_ck_stack_upmc": "stack",
    "skeleton_msqueue": "queue",
}


# ---------------------------------------------------------------------------
# Comparison table printing
# ---------------------------------------------------------------------------


def print_comparison_table(metrics_by_exe: Dict[str, RunMetrics]) -> None:
    """Print a DATA_STRUCTURES_1.md-style comparison table to stdout."""
    if not metrics_by_exe:
        return

    rows = []
    for exe_base, m in metrics_by_exe.items():
        ds_name = m.ds_name or EXE_TO_DS_NAME.get(exe_base, exe_base)
        ds_type = EXE_TO_DS_TYPE.get(exe_base, "?")
        rows.append(
            (
                ds_name,
                ds_type,
                m.lkmm_prod_ns,
                m.user_cons_ns,
                m.user_prod_ns,
                m.lkmm_cons_ns,
                m.e2e_ns,
                m.tput_mops,
                m.relay_pct,
            )
        )

    # Sort by E2E ascending
    rows.sort(key=lambda r: r[6])

    hdr = (
        f"{'Data Structure':<24s} {'Type':<7s} "
        f"{'LKMM Prod':>10s} {'User Cons':>10s} {'User Prod':>10s} {'LKMM Cons':>10s} "
        f"{'E2E (ns)':>9s} {'Tput':>10s} {'Relay':>7s}"
    )
    sep = "=" * len(hdr)

    print(f"\n{sep}")
    print("           DATA STRUCTURE COMPARISON")
    print(sep)
    print(hdr)
    print("-" * len(hdr))

    for ds_name, ds_type, lp, uc, up, lc, e2e, tput, relay in rows:
        print(
            f"{ds_name:<24s} {ds_type:<7s} "
            f"{lp:>10d} {uc:>10d} {up:>10d} {lc:>10d} "
            f"{e2e:>9d} {tput:>9.2f}M {relay:>6.0f}%"
        )

    print(sep)
    print("  Latencies: Avg-OK(ns). E2E = sum of 4 cols. Tput = 1e9/E2E.")
    print()


def print_comparison_table_median(
    all_run_metrics: Dict[str, List[RunMetrics]],
) -> None:
    """Print comparison table using median values across multiple runs."""
    if not all_run_metrics:
        return

    rows = []
    for exe_base, runs in all_run_metrics.items():
        if not runs:
            continue
        ds_name = runs[0].ds_name or EXE_TO_DS_NAME.get(exe_base, exe_base)
        ds_type = EXE_TO_DS_TYPE.get(exe_base, "?")

        lp = int(statistics.median([r.lkmm_prod_ns for r in runs]))
        uc = int(statistics.median([r.user_cons_ns for r in runs]))
        up = int(statistics.median([r.user_prod_ns for r in runs]))
        lc = int(statistics.median([r.lkmm_cons_ns for r in runs]))
        e2e = lp + uc + up + lc
        tput = (1e9 / e2e) / 1e6 if e2e > 0 else 0.0
        relay_vals = [r.relay_pct for r in runs if r.ku_popped > 0]
        relay = statistics.median(relay_vals) if relay_vals else 0.0

        rows.append((ds_name, ds_type, lp, uc, up, lc, e2e, tput, relay))

    rows.sort(key=lambda r: r[6])

    hdr = (
        f"{'Data Structure':<24s} {'Type':<7s} "
        f"{'LKMM Prod':>10s} {'User Cons':>10s} {'User Prod':>10s} {'LKMM Cons':>10s} "
        f"{'E2E (ns)':>9s} {'Tput':>10s} {'Relay':>7s}"
    )
    sep = "=" * len(hdr)

    num_runs = max(len(v) for v in all_run_metrics.values())
    print(f"\n{sep}")
    print(f"     DATA STRUCTURE COMPARISON (median of {num_runs} runs)")
    print(sep)
    print(hdr)
    print("-" * len(hdr))

    for ds_name, ds_type, lp, uc, up, lc, e2e, tput, relay in rows:
        print(
            f"{ds_name:<24s} {ds_type:<7s} "
            f"{lp:>10d} {uc:>10d} {up:>10d} {lc:>10d} "
            f"{e2e:>9d} {tput:>9.2f}M {relay:>6.0f}%"
        )

    print(sep)
    print("  Latencies: median Avg-OK(ns). E2E = sum of 4 cols. Tput = 1e9/E2E.")
    print()


# ---------------------------------------------------------------------------
# CSV output for --final mode
# ---------------------------------------------------------------------------


def write_final_csv(
    all_run_metrics: Dict[str, List[RunMetrics]],
    csv_path: str,
) -> None:
    """Write per-run metrics to a timestamped CSV."""
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "Index",
                "DS",
                "LKMM Prod (ns)",
                "User Cons (ns)",
                "User Prod (ns)",
                "LKMM Cons (ns)",
                "E2E (ns)",
                "Tput (Mop/s)",
                "Relay %",
            ]
        )
        idx = 0
        for exe_base, runs in all_run_metrics.items():
            for m in runs:
                idx += 1
                ds_name = m.ds_name or EXE_TO_DS_NAME.get(exe_base, exe_base)
                writer.writerow(
                    [
                        idx,
                        ds_name,
                        m.lkmm_prod_ns,
                        m.user_cons_ns,
                        m.user_prod_ns,
                        m.lkmm_cons_ns,
                        m.e2e_ns,
                        f"{m.tput_mops:.2f}",
                        f"{m.relay_pct:.0f}",
                    ]
                )
    print(f"CSV written to {csv_path}")


# ---------------------------------------------------------------------------
# CDF graph for --final mode
# ---------------------------------------------------------------------------


def generate_cdf_from_csv(csv_path: str, png_path: Optional[str] = None) -> None:
    """Read a --final CSV and generate a CDF graph from it.

    If *png_path* is None, derives it from *csv_path* by replacing the
    extension with ``_cdf.png``.
    """
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("Error: matplotlib/numpy not available, cannot generate graph.")
        return

    if png_path is None:
        base, _ = os.path.splitext(csv_path)
        png_path = f"{base}_cdf.png"

    # Read CSV into {ds_name: [e2e_ns, ...]}
    ds_e2e: Dict[str, List[int]] = {}
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            ds = row["DS"]
            e2e = int(row["E2E (ns)"])
            ds_e2e.setdefault(ds, []).append(e2e)

    if not ds_e2e:
        print(f"Error: no data rows found in {csv_path}")
        return

    _plot_cdf(ds_e2e, png_path)


def generate_cdf_graph(
    all_run_metrics: Dict[str, List[RunMetrics]],
    png_path: str,
) -> None:
    """Generate a CDF graph of E2E latency per data structure with percentile markers."""
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("Warning: matplotlib/numpy not available, skipping CDF graph.")
        return

    # Collect {ds_name: [e2e_ns, ...]}
    ds_e2e: Dict[str, List[int]] = {}
    for exe_base, runs in all_run_metrics.items():
        if not runs:
            continue
        ds_name = runs[0].ds_name or EXE_TO_DS_NAME.get(exe_base, exe_base)
        ds_e2e[ds_name] = [r.e2e_ns for r in runs]

    if not ds_e2e:
        return

    _plot_cdf(ds_e2e, png_path)


def _plot_cdf(ds_e2e: Dict[str, List[int]], png_path: str) -> None:
    """Shared CDF plotting logic used by both generate_cdf_graph and generate_cdf_from_csv."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    fig, ax = plt.subplots(figsize=(12, 7))

    # Sort data structures by median E2E
    ds_data = []
    for ds_name, vals in ds_e2e.items():
        ds_data.append((ds_name, sorted(vals)))

    ds_data.sort(key=lambda d: statistics.median(d[1]))

    # Color cycle
    colors = plt.cm.tab10.colors

    for i, (ds_name, vals) in enumerate(ds_data):
        n = len(vals)
        cdf_y = np.arange(1, n + 1) / n
        color = colors[i % len(colors)]
        ax.plot(vals, cdf_y, label=ds_name, color=color, linewidth=1.5)

    # Draw horizontal lines at p90, p95, p99
    for pct, style in [(0.90, "--"), (0.95, "-."), (0.99, ":")]:
        ax.axhline(
            y=pct,
            color="gray",
            linestyle=style,
            linewidth=0.8,
            label=f"p{int(pct * 100)}" if pct == 0.90 else f"p{int(pct * 100)}",
        )

    ax.set_xlabel("E2E Latency (ns)")
    ax.set_ylabel("Cumulative Probability")
    ax.set_title("E2E Latency CDF by Data Structure")
    ax.legend(loc="lower right", fontsize=8)
    ax.grid(True, alpha=0.3)
    ax.set_ylim(0, 1.02)

    plt.tight_layout()
    plt.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"CDF graph saved to {png_path}")


# ---------------------------------------------------------------------------
# Core runner infrastructure (unchanged logic, returns metrics now)
# ---------------------------------------------------------------------------


def find_executables() -> List[str]:
    """Find compiled executables in ./build."""
    candidates = [
        "skeleton_msqueue",
        "skeleton_vyukhov",
        "skeleton_folly_spsc",
        "skeleton_ck_fifo_spsc",
        "skeleton_ck_ring_spsc",
        "skeleton_ck_stack_upmc",
        "skeleton_io_uring",
        "skeleton_kcov",
        "skeleton_iouring_liburing",
    ]
    executables = []

    for exe in candidates:
        if os.path.isfile(os.path.join("build", exe)) and os.access(
            os.path.join("build", exe), os.X_OK
        ):
            executables.append(os.path.join("build", exe))
            print(f"Found executable: {exe}")

    return executables


def touch_file_worker(
    file_id: int, core_id: int, ready_event, start_event, stop_event, touch_pids
):
    """Worker process that continuously creates/deletes files on a specific core."""
    # Set CPU affinity to run on specific core
    try:
        os.sched_setaffinity(0, {core_id})
    except Exception as e:
        print(f"Warning: Could not set CPU affinity for worker {file_id}: {e}")

    # Signal ready and wait for start
    ready_event.set()
    start_event.wait()

    # Continuously create and delete files until stop signal
    filename = f"file{file_id}.tmp"
    count = 0
    while not stop_event.is_set():
        try:
            # Create new file (triggers inode_create) via subprocess (fork/exec)
            touch_process = subprocess.Popen(["touch", filename])
            touch_pids.append(touch_process.pid)
            touch_process.wait()
            count += 1
            # Small sleep to avoid overwhelming the system
            time.sleep(0.01)
            # Remove file to allow recreation
            if os.path.exists(filename):
                os.remove(filename)
        except Exception as e:
            pass  # Continue even if individual operations fail

    # Final cleanup
    try:
        if os.path.exists(filename):
            os.remove(filename)
    except:
        pass


def clear_trace_buffer():
    """Clear trace buffer so comparisons use fresh output only."""
    try:
        with open(TRACE_CLEAR_PATH, "w", encoding="utf-8") as trace_file:
            trace_file.write("")
    except Exception as e:
        print(f"Warning: Could not clear trace buffer: {e}")


def trace_pipe_reader(output_path: Path, stop_event: threading.Event):
    """Continuously copy trace_pipe output to a file until stopped."""
    trace_process = None
    with output_path.open("w", encoding="utf-8") as output_file:
        try:
            trace_process = subprocess.Popen(
                ["sudo", "cat", TRACE_PIPE_PATH],
                stdout=output_file,
                stderr=subprocess.DEVNULL,
                text=True,
            )

            while not stop_event.is_set():
                if trace_process.poll() is not None:
                    break
                time.sleep(0.05)
        except Exception as e:
            print(f"Warning: trace_pipe reader failed: {e}")
        finally:
            if trace_process and trace_process.poll() is None:
                trace_process.terminate()
                try:
                    trace_process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    trace_process.kill()
                    trace_process.wait()


def validate_trace_output(
    trace_log_path: Path, executable: str, produced_touch_pids: List[int]
) -> bool:
    """Compare consumed kernel output keys with the recorded touch process PIDs."""
    ds_name = os.path.basename(executable).replace("skeleton_", "")
    expected_counter = Counter(produced_touch_pids)

    consumed_keys = []
    try:
        trace_lines = trace_log_path.read_text(encoding="utf-8").splitlines()
    except Exception as e:
        print(f"  Trace validation FAILED: could not read {trace_log_path}: {e}")
        return False

    for line in trace_lines:
        if f"{ds_name} consume" not in line:
            continue
        match = TRACE_CONSUME_RE.search(line)
        if match:
            consumed_keys.append(int(match.group(1)))

    if not consumed_keys:
        print("  Trace validation FAILED: no consumer output captured in trace_pipe")
        return False

    consumed_counter = Counter(consumed_keys)
    unexpected_keys = consumed_counter - expected_counter
    if unexpected_keys:
        unexpected_total = sum(unexpected_keys.values())
        print(
            f"  Trace validation FAILED: {unexpected_total} consumed keys were not in initial touch inputs"
        )
        print(f"    Unexpected keys and counts: {dict(unexpected_keys)}")
        return False

    print(
        f"  Trace validation PASSED: {len(consumed_keys)} consumed events matched initial touch input PIDs"
    )
    print(f"  Trace output file: {trace_log_path}")
    return True


def run_executable_with_concurrent_touches(
    executable: str, duration: int = 10, show_output: bool = False
):
    """
    Run executable with concurrent file touches on separate cores.

    Args:
        executable: Path to the executable to run
        duration: Duration in seconds to run the test (program will be terminated after this)

    Returns:
        dict with run result info plus 'metrics' key containing RunMetrics (or None)
    """
    # nproc = multiprocessing.cpu_count()
    nproc = 1
    print(f"\n{'=' * 60}")
    print(f"Running: {executable} for {duration} seconds")
    print(f"Spawning {nproc} concurrent touch processes on separate cores")
    print(f"{'=' * 60}")

    # Create synchronization events
    manager = multiprocessing.Manager()
    start_event = manager.Event()
    stop_event = manager.Event()
    ready_events = [manager.Event() for _ in range(nproc)]
    touch_pids = manager.list()

    # Spawn worker processes
    processes = []
    for i in range(nproc):
        core_id = i % nproc  # Distribute across available cores
        p = multiprocessing.Process(
            target=touch_file_worker,
            args=(i, core_id, ready_events[i], start_event, stop_event, touch_pids),
        )
        p.start()
        processes.append(p)

    # Wait for all workers to be ready
    for event in ready_events:
        event.wait()

    print(f"All {nproc} worker processes ready on separate cores")

    clear_trace_buffer()
    trace_log_path = Path("build") / f"{Path(executable).name}_trace_pipe.log"
    trace_stop_event = threading.Event()
    trace_thread = threading.Thread(
        target=trace_pipe_reader,
        args=(trace_log_path, trace_stop_event),
        daemon=True,
    )
    trace_thread.start()

    # Start the executable
    start_time = time.time()
    exe_process = subprocess.Popen(
        [f"./{executable}"],
        stdout=None if show_output else subprocess.PIPE,
        stderr=None if show_output else subprocess.PIPE,
        text=True,
    )

    # Signal all workers to start creating files
    start_event.set()
    print(f"Signaled all workers to start creating files continuously")

    # Let it run for the specified duration
    time.sleep(duration)

    # Terminate the executable (it runs forever otherwise)
    # Give 15s for the skeleton to join relay thread, trigger kernel
    # consumer uprobe loop, and print statistics.
    exe_process.terminate()
    try:
        stdout, stderr = exe_process.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        exe_process.kill()
        stdout, stderr = exe_process.communicate()

    end_time = time.time()

    # Signal workers to stop
    stop_event.set()

    # Wait for all worker processes to complete
    for p in processes:
        p.join(timeout=2)

    # Stop trace capture and validate trace output against initial input PIDs
    trace_stop_event.set()
    trace_thread.join(timeout=3)
    trace_validation_ok = validate_trace_output(
        trace_log_path, executable, list(touch_pids)
    )

    # Results
    elapsed = end_time - start_time
    print(f"\nResults for {executable}:")
    print(f"  Return code: {exe_process.returncode}")
    print(f"  Elapsed time: {elapsed:.2f} seconds")

    if stdout:
        print(f"  STDOUT:\n{stdout}")
    if stderr:
        print(f"  STDERR:\n{stderr}")

    # Parse metrics from stdout
    run_metrics = parse_run_output(stdout) if stdout else None

    # Clean up any remaining temp files
    cleaned_count = 0
    for i in range(nproc):
        filename = f"file{i}.tmp"
        if os.path.exists(filename):
            try:
                os.remove(filename)
                cleaned_count += 1
            except Exception as e:
                print(f"  Warning: Could not remove {filename}: {e}")

    if cleaned_count > 0:
        print(f"  Cleaned up {cleaned_count} remaining temp files")

    return {
        "executable": executable,
        "return_code": exe_process.returncode,
        "elapsed_time": elapsed,
        "total_workers": nproc,
        "trace_validation_ok": trace_validation_ok,
        "metrics": run_metrics,
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        description="BPF arena executable test runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  # Run all data structures once, print comparison table\n"
            "  sudo python3 scripts/runner.py\n"
            "\n"
            "  # Run only msqueue and kcov\n"
            "  sudo python3 scripts/runner.py skeleton_msqueue skeleton_kcov\n"
            "\n"
            "  # Final benchmark: 100 runs per DS, write CSV + CDF graph\n"
            "  sudo python3 scripts/runner.py --final 100\n"
            "\n"
            "  # Generate CDF graph from an existing CSV\n"
            "  python3 scripts/runner.py --graph final_bench_20260413.csv\n"
        ),
    )
    parser.add_argument(
        "executables",
        nargs="*",
        help="Optional executable names/paths to run (e.g. skeleton_msqueue or build/skeleton_msqueue)",
    )
    parser.add_argument(
        "--show-output",
        action="store_true",
        help="Stream executable stdout/stderr live to the terminal",
    )
    parser.add_argument(
        "--final",
        type=int,
        nargs="?",
        const=1000,
        default=None,
        metavar="N",
        help=(
            "Final benchmark mode: run each data structure N times "
            "(default: 1000), output timestamped CSV and CDF graph"
        ),
    )
    parser.add_argument(
        "--duration",
        "-d",
        type=int,
        default=10,
        help="Duration in seconds per trial (default: 10)",
    )
    parser.add_argument(
        "--graph",
        type=str,
        default=None,
        metavar="CSV",
        help="Generate CDF graph from an existing --final CSV file (no benchmarks run)",
    )
    args = parser.parse_args()

    # --graph mode: just generate the graph and exit
    if args.graph is not None:
        if not os.path.isfile(args.graph):
            print(f"Error: CSV file not found: {args.graph}", file=sys.stderr)
            return 1
        generate_cdf_from_csv(args.graph)
        return 0

    print("BPF Arena Data Structures Test Runner")
    print("=" * 60)

    # Find executables
    executables = find_executables()

    if not executables:
        print(
            "No executables found! Expected: skeleton_msqueue, skeleton_vyukhov, etc."
        )
        return 1

    print(f"\nFound {len(executables)} executable(s)")

    if args.executables:
        # Filter executables based on command line arguments
        filter_set = set(args.executables)
        executables = [
            exe
            for exe in executables
            if exe in filter_set or os.path.basename(exe) in filter_set
        ]
        print(f"Filtered executables to run: {executables}")

    print(f"CPU count: {multiprocessing.cpu_count()}")

    if args.final is not None:
        return _run_final_mode(executables, args)
    else:
        return _run_normal_mode(executables, args)


def _run_normal_mode(executables: List[str], args) -> int:
    """Run each executable once, print comparison table at end."""
    results = []
    metrics_by_exe: Dict[str, RunMetrics] = {}

    for exe in executables:
        result = run_executable_with_concurrent_touches(
            exe,
            duration=args.duration,
            show_output=args.show_output,
        )
        results.append(result)

        if result["metrics"] is not None:
            exe_base = os.path.basename(exe)
            metrics_by_exe[exe_base] = result["metrics"]

    # Summary
    print(f"\n{'=' * 60}")
    print("SUMMARY")
    print(f"{'=' * 60}")
    for result in results:
        print(f"{result['executable']}:")
        print(f"  Success: {'Yes' if result['return_code'] == 0 else 'No'}")
        print(f"  Trace match: {'Yes' if result['trace_validation_ok'] else 'No'}")
        print(f"  Time: {result['elapsed_time']:.2f}s")
        print(f"  Workers: {result['total_workers']}")

    # Comparison table
    print_comparison_table(metrics_by_exe)

    return 0


def _run_final_mode(executables: List[str], args) -> int:
    """Run each executable N times, collect metrics, write CSV + CDF."""
    num_runs = args.final
    total_trials = len(executables) * num_runs
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    print(f"\n{'=' * 60}")
    print(f"  FINAL BENCHMARK MODE")
    print(f"  Runs per DS   : {num_runs}")
    print(f"  Duration/trial: {args.duration}s")
    print(f"  Total trials  : {total_trials}")
    print(f"  Est. time     : {total_trials * (args.duration + 5) / 3600:.1f} hours")
    print(f"{'=' * 60}\n")

    # Collect: exe_basename -> list of RunMetrics
    all_run_metrics: Dict[str, List[RunMetrics]] = {}

    trial_num = 0
    for exe in executables:
        exe_base = os.path.basename(exe)
        all_run_metrics[exe_base] = []

        for run_idx in range(num_runs):
            trial_num += 1
            print(
                f"\n  [{trial_num}/{total_trials}] "
                f"{exe_base}  run {run_idx + 1}/{num_runs}",
                flush=True,
            )

            result = run_executable_with_concurrent_touches(
                exe,
                duration=args.duration,
                show_output=args.show_output,
            )

            if result["metrics"] is not None:
                all_run_metrics[exe_base].append(result["metrics"])
                m = result["metrics"]
                print(
                    f"         -> E2E={m.e2e_ns}ns  "
                    f"Tput={m.tput_mops:.2f}Mop/s  "
                    f"Relay={m.relay_pct:.0f}%"
                )
            else:
                print("         -> WARNING: no metrics parsed")

    # Print median comparison table
    print_comparison_table_median(all_run_metrics)

    # Write CSV
    csv_path = f"final_bench_{timestamp}.csv"
    write_final_csv(all_run_metrics, csv_path)

    # Generate CDF graph
    png_path = f"final_bench_{timestamp}_cdf.png"
    generate_cdf_graph(all_run_metrics, png_path)

    return 0


if __name__ == "__main__":
    exit(main())
