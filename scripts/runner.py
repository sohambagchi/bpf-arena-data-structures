"""
Runner script for BPF arena data structure executables.
Spawns concurrent processes on separate cores to stress test during execution.
"""

import csv
import os
import subprocess
import multiprocessing
import time
import argparse
import threading
import re
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional


TRACE_PIPE_PATH = "/sys/kernel/debug/tracing/trace_pipe"
TRACE_CLEAR_PATH = "/sys/kernel/debug/tracing/trace"
TRACE_CONSUME_RE = re.compile(r"consume key=(\d+) value=(\d+)")

# Emitted by ds_metrics_print() (include/ds_metrics.h) when each relay exits.
METRICS_HEADER_RE = re.compile(r"PERFORMANCE METRICS:\s*(.+?)\s*$")
METRICS_ROW_RE = re.compile(
    r"^(LKMM producer|User consumer|User producer|LKMM consumer)\s+"
    r"(\d+)\s+(\d+)\s+([\d.]+)%\s+(\d+)\s+(\d+)\s+(\d+)\s*$"
)


def find_executables() -> List[str]:
    """Find compiled executables in ./build."""
    candidates = ['skeleton_msqueue', 'skeleton_vyukhov', 'skeleton_folly_spsc', 'skeleton_ck_fifo_spsc', 'skeleton_ck_ring_spsc', 'skeleton_ck_stack_upmc', 'skeleton_io_uring', 'skeleton_kcov']
    executables = []
    
    for exe in candidates:
        if os.path.isfile(os.path.join('build', exe)) and os.access(os.path.join('build', exe), os.X_OK):
            executables.append(os.path.join('build', exe))
            print(f"Found executable: {exe}")
    
    return executables


def touch_file_worker(file_id: int, core_id: int, ready_event, start_event, stop_event, touch_pids):
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
            touch_process = subprocess.Popen(['touch', filename])
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


def stream_reader(pipe, sink: List[str], echo: bool):
    """Drain a subprocess pipe into a list, optionally echoing it live."""
    try:
        for line in iter(pipe.readline, ''):
            sink.append(line)
            if echo:
                print(line, end='')
    except Exception as e:
        print(f"Warning: output reader failed: {e}")
    finally:
        try:
            pipe.close()
        except Exception:
            pass


def clear_trace_buffer():
    """Clear trace buffer so comparisons use fresh output only."""
    try:
        with open(TRACE_CLEAR_PATH, 'w', encoding='utf-8') as trace_file:
            trace_file.write('')
    except Exception as e:
        print(f"Warning: Could not clear trace buffer: {e}")


def trace_pipe_reader(output_path: Path, stop_event: threading.Event):
    """Continuously copy trace_pipe output to a file until stopped."""
    trace_process = None
    with output_path.open('w', encoding='utf-8') as output_file:
        try:
            trace_process = subprocess.Popen(
                ['sudo', 'cat', TRACE_PIPE_PATH],
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


def validate_trace_output(trace_log_path: Path, executable: str, produced_touch_pids: List[int]) -> bool:
    """Compare consumed kernel output keys with the recorded touch process PIDs."""
    ds_name = os.path.basename(executable).replace('skeleton_', '')
    expected_counter = Counter(produced_touch_pids)

    consumed_keys = []
    try:
        trace_lines = trace_log_path.read_text(encoding='utf-8').splitlines()
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
        print(f"  Trace validation FAILED: {unexpected_total} consumed keys were not in initial touch inputs")
        print(f"    Unexpected keys and counts: {dict(unexpected_keys)}")
        return False

    print(f"  Trace validation PASSED: {len(consumed_keys)} consumed events matched initial touch input PIDs")
    print(f"  Trace output file: {trace_log_path}")
    return True


def parse_metrics(stdout_text: str) -> Optional[Dict]:
    """Extract the ds_metrics_print() table from an executable's stdout.

    Every relay prints the same four-row table on exit (SIGTERM included, since
    print_stats defaults to true).  Each row carries the successful op count and
    the average latency of those successful ops, so the summed successful
    latency is recoverable as success * avg_ok.

    Returns None when no table was captured (e.g. the process had to be killed
    before it could flush its statistics).
    """
    ds_name = None
    categories = {}

    for line in stdout_text.splitlines():
        header = METRICS_HEADER_RE.search(line)
        if header:
            ds_name = header.group(1)
            continue

        row = METRICS_ROW_RE.match(line.strip())
        if not row:
            continue

        success = int(row.group(3))
        avg_ok_ns = int(row.group(6))
        categories[row.group(1)] = {
            'total': int(row.group(2)),
            'success': success,
            'avg_ns': int(row.group(5)),
            'avg_ok_ns': avg_ok_ns,
            'throughput': int(row.group(7)),
            # ds_metrics_print() reports avg_ok = success_latency_ns / success,
            # so this reconstructs the summed latency (modulo integer division).
            'success_latency_ns': success * avg_ok_ns,
        }

    if not categories:
        return None

    success_ops = sum(cat['success'] for cat in categories.values())
    success_latency_ns = sum(cat['success_latency_ns'] for cat in categories.values())

    # Aggregate throughput over all relay stages: successful ops per second of
    # time actually spent inside the data structure.
    ops_per_sec = (
        success_ops / (success_latency_ns / 1e9) if success_latency_ns > 0 else 0.0
    )

    # Cost of moving one item through every stage that did useful work.
    e2e_latency_ns = sum(
        cat['avg_ok_ns'] for cat in categories.values() if cat['success'] > 0
    )

    return {
        'ds_name': ds_name,
        'categories': categories,
        'success_ops': success_ops,
        'success_latency_ns': success_latency_ns,
        'ops_per_sec': ops_per_sec,
        'e2e_latency_ns': e2e_latency_ns,
    }


def export_metrics_csv(results: List[Dict], output_dir: Path = Path('build')) -> Optional[Path]:
    """Write the raw per-category metric numbers to a timestamped CSV file.

    One row per (data structure, relay stage), plus an 'ALL' row per data
    structure holding the aggregate figures used for ranking.  Values are the
    absolute numbers reported by ds_metrics_print(), not derived percentages.
    """
    timestamp = time.strftime('%Y%m%d-%H%M%S')
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / f"runner_metrics_{timestamp}.csv"

    columns = [
        'timestamp', 'executable', 'ds_name', 'category',
        'total_ops', 'success_ops', 'avg_latency_ns', 'avg_success_latency_ns',
        'success_latency_ns', 'throughput_ops_per_sec', 'e2e_latency_ns',
        'return_code', 'trace_validation_ok', 'wall_elapsed_s', 'workers',
    ]

    try:
        with csv_path.open('w', newline='', encoding='utf-8') as csv_file:
            writer = csv.writer(csv_file)
            writer.writerow(columns)

            for result in results:
                common = [
                    timestamp,
                    result['executable'],
                    result['ds_name'],
                ]
                tail = [
                    result['return_code'],
                    int(bool(result['trace_validation_ok'])),
                    f"{result['elapsed_time']:.3f}",
                    result['total_workers'],
                ]

                metrics = result.get('metrics')
                if not metrics:
                    writer.writerow(common + ['NO_METRICS'] + [''] * 7 + tail)
                    continue

                for category, values in metrics['categories'].items():
                    writer.writerow(common + [
                        category,
                        values['total'],
                        values['success'],
                        values['avg_ns'],
                        values['avg_ok_ns'],
                        values['success_latency_ns'],
                        values['throughput'],
                        '',
                    ] + tail)

                writer.writerow(common + [
                    'ALL',
                    sum(v['total'] for v in metrics['categories'].values()),
                    metrics['success_ops'],
                    '',
                    '',
                    metrics['success_latency_ns'],
                    f"{metrics['ops_per_sec']:.3f}",
                    metrics['e2e_latency_ns'],
                ] + tail)
    except Exception as e:
        print(f"Warning: could not write metrics CSV {csv_path}: {e}")
        return None

    return csv_path


def print_performance_ranking(results: List[Dict]):
    """Print data structure names in ascending performance order (slowest first)."""
    print(f"\n{'='*60}")
    print("PERFORMANCE RANKING (slowest first)")
    print(f"{'='*60}")
    print("Ranked by aggregate successful ops/sec across all four relay stages")
    print("(LKMM producer, user consumer, user producer, LKMM consumer).\n")

    ranked = [r for r in results if r.get('metrics')]
    unranked = [r for r in results if not r.get('metrics')]

    if not ranked:
        print("No performance metrics captured for any data structure.")
    else:
        ranked.sort(key=lambda r: r['metrics']['ops_per_sec'])

        print(f"{'#':>2}  {'Data structure':<20} {'Ops/sec':>14} "
              f"{'End-to-end(ns)':>15} {'Ops-OK':>10}")
        for position, result in enumerate(ranked, start=1):
            metrics = result['metrics']
            print(f"{position:>2}. {result['ds_name']:<20} "
                  f"{metrics['ops_per_sec']:>14,.0f} "
                  f"{metrics['e2e_latency_ns']:>15,} "
                  f"{metrics['success_ops']:>10,}")

        print("\nAscending performance order (slowest first):")
        print("  " + " < ".join(r['ds_name'] for r in ranked))

    if unranked:
        print("\nNo metrics captured (excluded from ranking): "
              + ", ".join(r['ds_name'] for r in unranked))


def run_executable_with_concurrent_touches(executable: str, duration: int = 10, show_output: bool = False):
    """
    Run executable with concurrent file touches on separate cores.
    
    Args:
        executable: Path to the executable to run
        duration: Duration in seconds to run the test (program will be terminated after this)
    """
    # nproc = multiprocessing.cpu_count()
    nproc = 1
    print(f"\n{'='*60}")
    print(f"Running: {executable} for {duration} seconds")
    print(f"Spawning {nproc} concurrent touch processes on separate cores")
    print(f"{'='*60}")
    
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
            args=(i, core_id, ready_events[i], start_event, stop_event, touch_pids)
        )
        p.start()
        processes.append(p)
    
    # Wait for all workers to be ready
    for event in ready_events:
        event.wait()
    
    print(f"All {nproc} worker processes ready on separate cores")
    
    clear_trace_buffer()
    trace_log_path = Path('build') / f"{Path(executable).name}_trace_pipe.log"
    trace_stop_event = threading.Event()
    trace_thread = threading.Thread(
        target=trace_pipe_reader,
        args=(trace_log_path, trace_stop_event),
        daemon=True,
    )
    trace_thread.start()

    # Start the executable
    start_time = time.time()
    # Always capture stdout/stderr so the exit-time metrics table can be parsed;
    # with --show-output the reader threads echo it live as well.
    exe_process = subprocess.Popen(
        [f'./{executable}'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    stdout_lines: List[str] = []
    stderr_lines: List[str] = []
    output_threads = [
        threading.Thread(
            target=stream_reader,
            args=(exe_process.stdout, stdout_lines, show_output),
            daemon=True,
        ),
        threading.Thread(
            target=stream_reader,
            args=(exe_process.stderr, stderr_lines, show_output),
            daemon=True,
        ),
    ]
    for thread in output_threads:
        thread.start()

    # Signal all workers to start creating files
    start_event.set()
    print(f"Signaled all workers to start creating files continuously")
    
    # Let it run for the specified duration
    time.sleep(duration)
    
    # Terminate the executable (it runs forever otherwise).  SIGTERM is the
    # graceful path: each skeleton drains its relay and prints its metrics table.
    exe_process.terminate()
    try:
        exe_process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        exe_process.kill()
        exe_process.wait()

    for thread in output_threads:
        thread.join(timeout=2)

    stdout = ''.join(stdout_lines)
    stderr = ''.join(stderr_lines)

    end_time = time.time()
    
    # Signal workers to stop
    stop_event.set()
    
    # Wait for all worker processes to complete
    for p in processes:
        p.join(timeout=2)

    # Stop trace capture and validate trace output against initial input PIDs
    trace_stop_event.set()
    trace_thread.join(timeout=3)
    trace_validation_ok = validate_trace_output(trace_log_path, executable, list(touch_pids))
    
    # Results
    elapsed = end_time - start_time
    print(f"\nResults for {executable}:")
    print(f"  Return code: {exe_process.returncode}")
    print(f"  Elapsed time: {elapsed:.2f} seconds")
    
    if not show_output:
        # Already echoed live by the reader threads when show_output is set.
        if stdout:
            print(f"  STDOUT:\n{stdout}")
        if stderr:
            print(f"  STDERR:\n{stderr}")

    metrics = parse_metrics(stdout)
    if metrics:
        print(f"  Aggregate throughput: {metrics['ops_per_sec']:,.0f} ops/sec "
              f"over {metrics['success_ops']:,} successful ops")
    else:
        print("  Warning: no performance metrics table captured")

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
        'executable': executable,
        'ds_name': (metrics or {}).get('ds_name')
        or os.path.basename(executable).replace('skeleton_', ''),
        'return_code': exe_process.returncode,
        'elapsed_time': elapsed,
        'total_workers': nproc,
        'trace_validation_ok': trace_validation_ok,
        'metrics': metrics,
    }


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(description="BPF arena executable test runner")
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
        "--csv-dir",
        default="build",
        help="Directory for the timestamped raw-metrics CSV (default: build)",
    )
    args = parser.parse_args()

    print("BPF Arena Data Structures Test Runner")
    print("=" * 60)
    
    # Find executables
    executables = find_executables()
    
    if not executables:
        print("No executables found! Expected: skeleton, skeleton_msqueue, skeleton_vyukhov")
        return 1
    
    print(f"\nFound {len(executables)} executable(s)")
    
    if args.executables:
        # Filter executables based on command line arguments
        filter_set = set(args.executables)
        executables = [
            exe for exe in executables
            if exe in filter_set or os.path.basename(exe) in filter_set
        ]
        print(f"Filtered executables to run: {executables}")
    
    print(f"CPU count: {multiprocessing.cpu_count()}")
    
    # Run each executable
    results = []
    for exe in executables:
        result = run_executable_with_concurrent_touches(
            exe,
            duration=10,
            show_output=args.show_output,
        )
        results.append(result)
    
    # Summary
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    for result in results:
        print(f"{result['executable']}:")
        print(f"  Success: {'Yes' if result['return_code'] == 0 else 'No'}")
        print(f"  Trace match: {'Yes' if result['trace_validation_ok'] else 'No'}")
        print(f"  Time: {result['elapsed_time']:.2f}s")
        print(f"  Workers: {result['total_workers']}")

    csv_path = export_metrics_csv(results, Path(args.csv_dir))
    if csv_path:
        print(f"\nRaw metrics CSV: {csv_path}")

    print_performance_ranking(results)

    return 0


if __name__ == '__main__':
    exit(main())
