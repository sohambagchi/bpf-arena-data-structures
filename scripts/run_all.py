#!/usr/bin/env python3
"""Single orchestration endpoint for the whole artifact pipeline.

Runs, in order:

    1. kconfig    python3 scripts/check_kconfig.py   -- kernel supports the relays
    2. usertests  python3 scripts/usertests.py       -- userspace DS correctness
    3. runner     python3 scripts/runner.py          -- eight relays + ranking + CSV

Building is deliberately *not* one of them.  `make` is an explicit step you run
first, as your normal user; this script only runs what it left in build/ and
fails up front if those binaries are missing.  The reason is sudo, which the
runner stage needs: sudo resets the environment, so a build launched through it
loses the dev shell's compiler settings -- inside `nix develop` that is
NIX_CFLAGS_COMPILE and PKG_CONFIG_PATH, without which the very same `make`
fails on "libelf.h: No such file or directory".  It would also leave root-owned
objects in build/ and .output/ that the next unprivileged `make` cannot
overwrite.

Each stage runs as a subprocess with its output streamed live and captured, so
a failure can be reported loudly (banner + output tail + hint) instead of
scrolling past.  By default the pipeline stops at the first failing stage,
since every later stage depends on the earlier ones: an unmet kernel config
makes the relays unattachable, and a failing usertest makes their numbers
meaningless.

Usage:
    make                                           # separate, unprivileged step
    sudo python3 scripts/run_all.py                # full pipeline; relays need root
    python3 scripts/run_all.py --only usertests    # one stage
    python3 scripts/run_all.py --skip kconfig
    python3 scripts/run_all.py --keep-going        # run everything, report at end

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
    "kconfig": (
        "This kernel cannot run the arena relays.\n"
        "  - Apply kernel/configs/delta-bpf-arena.config and rebuild, or boot a\n"
        "    kernel that already satisfies it; the report above lists each\n"
        "    unmet symbol.\n"
        "  - To check a different config: "
        "python3 scripts/run_all.py --config path/to/.config\n"
        "  - The later stages can still be run against a compliant kernel later."
    ),
    "usertests": (
        "A userspace data structure test failed -- the relay measurements would\n"
        "  be meaningless, so the pipeline stopped here.\n"
        "  - See which ones: python3 scripts/usertests.py --keep-going\n"
        "  - These are pure pthread tests; a failure is a data structure bug,\n"
        "    not a kernel or BPF configuration problem."
    ),
    "runner": (
        "The relay run failed.\n"
        "  - It needs root (BPF program load + trace_pipe): "
        "sudo python3 scripts/run_all.py --only runner\n"
        "  - It also needs debugfs mounted: "
        "sudo mount -t debugfs none /sys/kernel/debug\n"
        "  - Confirm the kernel side first: python3 scripts/check_kconfig.py\n"
        "  - In Docker, a plain `docker run` cannot do this no matter how root\n"
        "    you are inside it: seccomp blocks bpf(2), CAP_BPF/CAP_PERFMON are\n"
        "    absent, and debugfs, bpffs, securityfs and /boot are unmounted. Start\n"
        "    the container from the host with the wrapper that supplies all of\n"
        "    them:  scripts/run-docker.sh -- python3 scripts/run_all.py"
    ),
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


def report_stage_failure(result: Dict, tail_lines: int = 20):
    """Print a loud, self-contained failure report for one stage."""
    stage = result['stage']

    print(banner([
        f"STAGE FAILED: {stage}",
        "",
        f"command    : {result['command']}",
        f"exit status: {result['return_code']}",
        f"elapsed    : {result['elapsed']:.1f}s",
    ]), flush=True)

    tail = [line.rstrip() for line in result['output'].splitlines() if line.strip()]
    if tail:
        print(f"  Last {min(tail_lines, len(tail))} non-empty line(s) of {stage} output:")
        for line in tail[-tail_lines:]:
            print(f"    | {line}")
        print()

    hint = STAGE_HINTS.get(stage)
    if hint:
        print(f"  What to do:\n  {hint}\n", flush=True)


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
    """Fail before the first stage if `make` has not been run.

    Building is a separate step on purpose (see the module docstring), so the
    binaries either exist or the pipeline has nothing to run.  Saying so here,
    naming every missing one at once, beats letting usertests.py find an empty
    build/ and runner.py then report "No executables found!".
    """
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

    lines = ["ERROR: the binaries are not built", "",
             "run_all.py builds nothing; it runs what `make` leaves in build/.",
             "", "Missing:"]
    lines += [f"  {entry}" for entry in missing]
    lines += [
        "",
        "Build first, as your normal user -- not under sudo:",
        "",
        "  make",
        "  sudo python3 scripts/run_all.py",
        "",
        "sudo resets the environment, so a build started through it loses the",
        "dev shell's compiler settings: inside `nix develop` that is",
        "NIX_CFLAGS_COMPILE and PKG_CONFIG_PATH, and the compile then fails on",
        '"libelf.h: No such file or directory" even though the same `make`',
        "succeeds as your normal user.  It also leaves root-owned objects that",
        "the next unprivileged `make` cannot overwrite.",
        "",
        "If `make` itself is what failed:",
        "  - libbpf/bpftool are submodules: git submodule update --init --recursive",
        "  - needs clang-20 (or set CLANG=clang) with BPF arena support",
        "  - re-run it verbosely: make V=1",
    ]
    print(banner(lines), file=sys.stderr, flush=True)
    return False


def check_privileges(stages: List[str]) -> None:
    """Warn up front -- before the earlier stages run -- if the relay stage will not work."""
    if "runner" not in stages:
        return
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return

    print(banner([
        "WARNING: not running as root",
        "",
        "The 'runner' stage loads BPF programs and reads trace_pipe, both of",
        "which require root. It will very likely fail at the end of this run.",
        "",
        "Re-run as:  sudo python3 scripts/run_all.py",
    ], char="!"), flush=True)


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
        lines.append(f"{result['stage']:<12} exit {result['return_code']}  "
                     f"({result['command']})")
    lines += ["", "See the STAGE FAILED report(s) above for the output tail and",
              "the suggested fix for each."]
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
    print("BPF Arena Data Structures -- full pipeline")
    print(f"{'='*BANNER_WIDTH}")
    print(f"Repo root : {root}")
    print(f"Stages    : {' -> '.join(stages)}")
    print(f"On failure: {'continue' if args.keep_going else 'stop'}")

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
