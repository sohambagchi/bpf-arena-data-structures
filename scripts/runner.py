"""
Runner script for BPF arena data structure executables.
Spawns concurrent processes on separate cores to stress test during execution.
"""

import os
import subprocess
import multiprocessing
import time
from pathlib import Path
from typing import List


def find_executables() -> List[str]:
    """Find compiled executables in current directory."""
    candidates = ['skeleton', 'skeleton_msqueue', 'skeleton_vyukhov', 'skeleton_mpsc']
    executables = []
    
    for exe in candidates:
        if os.path.isfile(exe) and os.access(exe, os.X_OK):
            executables.append(exe)
            print(f"Found executable: {exe}")
    
    return executables


def touch_file_worker(file_id: int, core_id: int, ready_event, start_event, stop_event):
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
            # Create new file (triggers inode_create)
            Path(filename).touch()
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


def run_executable_with_concurrent_touches(executable: str, duration: int = 10):
    """
    Run executable with concurrent file touches on separate cores.
    
    Args:
        executable: Path to the executable to run
        duration: Duration in seconds to run the test (program will be terminated after this)
    """
    nproc = multiprocessing.cpu_count()
    print(f"\n{'='*60}")
    print(f"Running: {executable} for {duration} seconds")
    print(f"Spawning {nproc} concurrent touch processes on separate cores")
    print(f"{'='*60}")
    
    # Create synchronization events
    manager = multiprocessing.Manager()
    start_event = manager.Event()
    stop_event = manager.Event()
    ready_events = [manager.Event() for _ in range(nproc)]
    
    # Spawn worker processes
    processes = []
    for i in range(nproc):
        core_id = i % nproc  # Distribute across available cores
        p = multiprocessing.Process(
            target=touch_file_worker,
            args=(i, core_id, ready_events[i], start_event, stop_event)
        )
        p.start()
        processes.append(p)
    
    # Wait for all workers to be ready
    for event in ready_events:
        event.wait()
    
    print(f"All {nproc} worker processes ready on separate cores")
    
    # Start the executable
    start_time = time.time()
    exe_process = subprocess.Popen(
        [f'./{executable}'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
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
        'total_workers': nproc
    }


def main():
    """Main entry point."""
    print("BPF Arena Data Structures Test Runner")
    print("=" * 60)
    
    # Find executables
    executables = find_executables()
    
    if not executables:
        print("No executables found! Expected: skeleton, skeleton_msqueue, skeleton_vyukhov")
        return 1
    
    print(f"\nFound {len(executables)} executable(s)")
    print(f"CPU count: {multiprocessing.cpu_count()}")
    
    # Run each executable
    results = []
    for exe in executables:
        result = run_executable_with_concurrent_touches(exe, duration=10)
        results.append(result)
    
    # Summary
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    for result in results:
        print(f"{result['executable']}:")
        print(f"  Success: {'Yes' if result['return_code'] == 0 else 'No'}")
        print(f"  Time: {result['elapsed_time']:.2f}s")
        print(f"  Workers: {result['total_workers']}")
    
    return 0


if __name__ == '__main__':
    exit(main())
