#!/usr/bin/env python3
"""Run the artifact pipeline: kconfig -> usertests -> runner.

Builds nothing: `make` is a separate, unprivileged step, and this script runs
the binaries it left in build/.  Stops at the first failing stage unless
--keep-going, since each stage depends on the ones before it.

Usage:
    make                                           # first, as your normal user
    sudo python3 scripts/run_all.py                # the relays need root
    python3 scripts/run_all.py --only usertests    # one stage

Exit status: 0 if every executed stage passed, 1 otherwise.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional

STAGES = ["kconfig", "usertests", "runner"]

STAGE_BINARIES = {
    "usertests": "USERTEST_APPS",
    "runner": "BPF_APPS",
}

BANNER_WIDTH = 72

STAGE_HINTS = {
    "kconfig": "Boot a kernel that satisfies it: ./scripts/build-kernel.sh --base running -y",
    "usertests": "A pure pthread test failed, so this is a data structure bug, "
                 "not a kernel or BPF problem.",
    "runner": "The relays need root, debugfs mounted, and the kernel from "
              "check_kconfig.py. In Docker: scripts/init-docker.sh -- python3 scripts/run_all.py",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def banner(lines: List[str], char: str = "#") -> str:
    """Build a boxed, hard-to-miss block of text."""
    rule = char * BANNER_WIDTH
    body = "\n".join(f"{char*2}  {line}" for line in lines)
    return f"\n{rule}\n{body}\n{rule}\n"


def stream_reader(pipe, sink: List[str]):
    """Drain a subprocess pipe into a list while echoing it live."""
    try:
        for line in iter(pipe.readline, ''):
            sink.append(line)
            sys.stdout.write(line)
            sys.stdout.flush()
    except Exception as e:
        print(f"Warning: output reader failed: {e}")
    finally:
        try:
            pipe.close()
        except Exception:
            pass


def run_stage(name: str, cmd: List[str], cwd: Path, dry_run: bool = False) -> Dict:
    """Run one stage, teeing its output. Returns a result record."""
    printable = " ".join(cmd)

    print(f"\n{'='*BANNER_WIDTH}")
    print(f"STAGE: {name}")
    print(f"$ {printable}")
    print(f"{'='*BANNER_WIDTH}", flush=True)

    if dry_run:
        return {'stage': name, 'command': printable, 'status': 'skipped',
                'return_code': 0, 'elapsed': 0.0, 'output': ''}

    start = time.time()
    try:
        process = subprocess.Popen(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except OSError as e:
        return {'stage': name, 'command': printable, 'status': 'failed',
                'return_code': 127, 'elapsed': 0.0,
                'output': f"could not execute {cmd[0]}: {e}\n"}

    output: List[str] = []
    reader = threading.Thread(target=stream_reader, args=(process.stdout, output), daemon=True)
    reader.start()

    try:
        process.wait()
    except KeyboardInterrupt:
        process.terminate()
        process.wait()
        reader.join(timeout=2)
        return {'stage': name, 'command': printable, 'status': 'interrupted',
                'return_code': 130, 'elapsed': time.time() - start,
                'output': ''.join(output)}

    reader.join(timeout=5)
    elapsed = time.time() - start

    return {
        'stage': name,
        'command': printable,
        'status': 'passed' if process.returncode == 0 else 'failed',
        'return_code': process.returncode,
        'elapsed': elapsed,
        'output': ''.join(output),
    }


def report_stage_failure(result: Dict, tail_lines: int = 10):
    """Print a short failure report for one stage."""
    stage = result['stage']

    print(banner([f"STAGE FAILED: {stage} (exit {result['return_code']})",
                  result['command']]), flush=True)

    tail = [line.rstrip() for line in result['output'].splitlines() if line.strip()]
    for line in tail[-tail_lines:]:
        print(f"    | {line}")

    hint = STAGE_HINTS.get(stage)
    if hint:
        print(f"\n  {hint}\n", flush=True)


def newest_csv_files(csv_dir: Path, before: set) -> List[Path]:
    """CSV files that appeared in csv_dir during the runner stage."""
    try:
        after = set(csv_dir.glob("runner_metrics_*.csv"))
    except Exception:
        return []
    return sorted(after - before)


def makefile_apps(root: Path, variable: str) -> List[str]:
    """The app names on one `VAR = a b c` line of the Makefile, in order."""
    try:
        text = (root / "Makefile").read_text(encoding="utf-8")
    except OSError:
        return []
    match = re.search(rf"^{variable}\s*=\s*(.+?)\s*$", text, flags=re.MULTILINE)
    return match.group(1).split() if match else []


def check_binaries(stages: List[str], root: Path) -> bool:
    """Fail before the first stage, naming every missing binary at once."""
    missing: List[str] = []
    for stage in stages:
        variable = STAGE_BINARIES.get(stage)
        if not variable:
            continue
        for app in makefile_apps(root, variable):
            binary = root / "build" / app
            if not (binary.is_file() and os.access(binary, os.X_OK)):
                missing.append(f"build/{app}  ({stage})")

    if not missing:
        return True

    lines = ["ERROR: not built -- run `make` first, as your normal user", "", "Missing:"]
    lines += [f"  {entry}" for entry in missing]
    print(banner(lines), file=sys.stderr, flush=True)
    return False


def check_privileges(stages: List[str]) -> None:
    """Warn up front if the relay stage will not work."""
    if "runner" not in stages:
        return
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return

    print(banner(["WARNING: not root; the 'runner' stage will fail",
                  "Re-run as: sudo python3 scripts/run_all.py"], char="!"), flush=True)


def print_summary(results: List[Dict], csv_files: List[Path]) -> None:
    print(f"\n{'='*BANNER_WIDTH}")
    print("PIPELINE SUMMARY")
    print(f"{'='*BANNER_WIDTH}")

    labels = {'passed': 'PASS', 'failed': 'FAIL',
              'skipped': 'SKIP', 'interrupted': 'INTR'}

    for result in results:
        label = labels.get(result['status'], result['status'].upper())
        detail = "" if result['status'] in ('passed', 'skipped') \
            else f"  (exit {result['return_code']})"
        print(f"  [{label}] {result['stage']:<12} {result['elapsed']:>7.1f}s{detail}")

    if csv_files:
        print("\n  Raw metrics CSV:")
        for path in csv_files:
            print(f"    {path}")

    failed = [r for r in results if r['status'] in ('failed', 'interrupted')]
    if not failed:
        print(banner(["PIPELINE PASSED", "",
                      f"All {len(results)} stage(s) completed successfully."], char="="))
        return

    lines = [f"PIPELINE FAILED: {len(failed)} of {len(results)} stage(s)", ""]
    for result in failed:
        lines.append(f"{result['stage']:<12} exit {result['return_code']}")
    print(banner(lines))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the artifact pipeline: kconfig -> usertests -> runner. "
                    "Build first with `make`; this script runs the binaries "
                    "that leaves in build/.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--only", choices=STAGES, action="append", default=None,
                        help="Run only this stage (repeatable)")
    parser.add_argument("--skip", choices=STAGES, action="append", default=[],
                        help="Skip this stage (repeatable)")
    parser.add_argument("--keep-going", action="store_true",
                        help="Run remaining stages after a failure instead of stopping")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the stage commands without executing them")

    parser.add_argument("--config", default=None,
                        help="kconfig stage: path to a .config "
                             "(default: /boot/config-$(uname -r))")
    parser.add_argument("--usertest-timeout", type=int, default=None,
                        help="usertests stage: per-test timeout in seconds")
    parser.add_argument("--csv-dir", default="build",
                        help="runner stage: directory for the timestamped metrics CSV")
    parser.add_argument("--show-output", action="store_true",
                        help="runner stage: stream each relay's stdout/stderr live")
    parser.add_argument("executables", nargs="*",
                        help="runner stage: limit to these relays "
                             "(e.g. skeleton_msqueue)")

    args = parser.parse_args()

    root = repo_root()
    python = sys.executable or "python3"

    selected = args.only if args.only else list(STAGES)
    stages = [s for s in STAGES if s in selected and s not in args.skip]

    if not stages:
        print("error: every stage was skipped; nothing to do", file=sys.stderr)
        return 1

    if not args.dry_run and not check_binaries(stages, root):
        return 1

    commands: Dict[str, List[str]] = {
        "kconfig": [python, "scripts/check_kconfig.py"]
                   + ([args.config] if args.config else []),
        "usertests": [python, "scripts/usertests.py", "--keep-going"]
                     + (["--timeout", str(args.usertest_timeout)]
                        if args.usertest_timeout else []),
        "runner": [python, "scripts/runner.py", "--csv-dir", args.csv_dir]
                  + (["--show-output"] if args.show_output else [])
                  + list(args.executables),
    }

    print(f"{'='*BANNER_WIDTH}")
    print(f"BPF Arena Data Structures -- {' -> '.join(stages)}")
    print(f"{'='*BANNER_WIDTH}")

    if not args.dry_run:
        check_privileges(stages)

    csv_dir = Path(args.csv_dir)
    if not csv_dir.is_absolute():
        csv_dir = root / csv_dir
    try:
        csv_before = set(csv_dir.glob("runner_metrics_*.csv"))
    except Exception:
        csv_before = set()

    results: List[Dict] = []
    failed_early: Optional[str] = None

    for stage in stages:
        result = run_stage(stage, commands[stage], root, dry_run=args.dry_run)
        results.append(result)

        if result['status'] in ('failed', 'interrupted'):
            report_stage_failure(result)
            if not args.keep_going:
                failed_early = stage
                break

    if failed_early:
        remaining = stages[stages.index(failed_early) + 1:]
        if remaining:
            print(f"  Not run because '{failed_early}' failed: "
                  f"{', '.join(remaining)}")
            print("  Use --keep-going to run them anyway.\n")

    csv_files = newest_csv_files(csv_dir, csv_before) if not args.dry_run else []
    print_summary(results, csv_files)

    return 0 if all(r['status'] in ('passed', 'skipped') for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
