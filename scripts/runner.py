"""
Runner script for BPF arena data structure executables.
Spawns concurrent processes on separate cores to stress test during execution.
"""

import os
import subprocess
import multiprocessing
import time
import argparse
import threading
import re
from collections import Counter
from pathlib import Path
from typing import List


TRACE_PIPE_PATH = "/sys/kernel/debug/tracing/trace_pipe"
TRACE_CLEAR_PATH = "/sys/kernel/debug/tracing/trace"
TRACE_CONSUME_RE = re.compile(r"consume key=(\d+) value=(\d+)")


def find_executables() -> List[str]:
    """Find compiled executables in ./build."""
    candidates = ['skeleton_msqueue', 'skeleton_vyukhov', 'skeleton_folly_spsc', 'skeleton_ck_fifo_spsc', 'skeleton_ck_ring_spsc', 'skeleton_ck_stack_upmc']
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
    exe_process = subprocess.Popen(
        [f'./{executable}'],
        stdout=None if show_output else subprocess.PIPE,
        stderr=None if show_output else subprocess.PIPE,
        text=True
    )
    
    # Signal all workers to start creating files
    start_event.set()
    print(f"Signaled all workers to start creating files continuously")
    
    # Let it run for the specified duration
    time.sleep(duration)
    
    # Terminate the executable (it runs forever otherwise)
    exe_process.terminate()
    try:
        stdout, stderr = exe_process.communicate(timeout=2)
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
    trace_validation_ok = validate_trace_output(trace_log_path, executable, list(touch_pids))
    
    # Results
    elapsed = end_time - start_time
    print(f"\nResults for {executable}:")
    print(f"  Return code: {exe_process.returncode}")
    print(f"  Elapsed time: {elapsed:.2f} seconds")
    
    if stdout:
        print(f"  STDOUT:\n{stdout}")
    if stderr:
        print(f"  STDERR:\n{stderr}")
    
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
        'return_code': exe_process.returncode,
        'elapsed_time': elapsed,
        'total_workers': nproc,
        'trace_validation_ok': trace_validation_ok,
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
    
    return 0


if __name__ == '__main__':
    exit(main())
