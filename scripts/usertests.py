#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_TIMEOUT_SEC = 120


PRODUCER_RE = re.compile(r"^producer(?:\[\d+\])?: key=(\d+) value=(\d+)\b")
CONSUMER_RE = re.compile(r"^consumer: key=(\d+) value=(\d+)\b")
CONSUMER_FINAL_RE = re.compile(r"^consumer-final: key=(\d+) value=(\d+)\b")
DONE_RE = re.compile(r"^done: produced=(\d+) consumed=(\d+)\b")


def repo_root() -> Path:
	return Path(__file__).resolve().parents[1]


def parse_makefile_usertest_apps(makefile: Path) -> list[str] | None:
	try:
		text = makefile.read_text(encoding="utf-8")
	except OSError:
		return None

	m = re.search(r"^USERTEST_APPS\s*=\s*(.+?)\s*$", text, flags=re.MULTILINE)
	if not m:
		return None
	parts = [p.strip() for p in m.group(1).split() if p.strip()]
	return parts or None


def default_apps() -> list[str]:
	root = repo_root()
	apps = parse_makefile_usertest_apps(root / "Makefile")
	if apps:
		return apps
	return [
		"usertest_list",
		"usertest_msqueue",
		"usertest_mpsc",
		"usertest_vyukhov",
		"usertest_folly_spsc",
		"usertest_bst",
		"usertest_bintree",
	]


@dataclass(frozen=True)
class ParsedOutput:
	produced_pairs: list[tuple[int, int]]
	consumed_pairs: list[tuple[int, int]]
	done_produced: int | None
	done_consumed: int | None
	error_markers: list[str]


def parse_output(text: str) -> ParsedOutput:
	produced: list[tuple[int, int]] = []
	consumed: list[tuple[int, int]] = []
	done_produced: int | None = None
	done_consumed: int | None = None
	error_markers: list[str] = []

	for line in text.splitlines():
		line = line.strip()
		if not line:
			continue

		m = PRODUCER_RE.match(line)
		if m:
			produced.append((int(m.group(1)), int(m.group(2))))
			continue

		m = CONSUMER_RE.match(line)
		if m:
			consumed.append((int(m.group(1)), int(m.group(2))))
			continue

		m = CONSUMER_FINAL_RE.match(line)
		if m:
			consumed.append((int(m.group(1)), int(m.group(2))))
			continue

		m = DONE_RE.match(line)
		if m:
			done_produced = int(m.group(1))
			done_consumed = int(m.group(2))
			continue

		lower = line.lower()
		if " timeout " in f" {lower} ":
			error_markers.append("timeout")
		if " rc=" in line:
			error_markers.append("rc=")
		if lower.startswith("error:") or lower.startswith("fatal:"):
			error_markers.append("error:")

	return ParsedOutput(
		produced_pairs=produced,
		consumed_pairs=consumed,
		done_produced=done_produced,
		done_consumed=done_consumed,
		error_markers=error_markers,
	)


def multiset_eq(a: Iterable[tuple[int, int]], b: Iterable[tuple[int, int]]) -> bool:
	return Counter(a) == Counter(b)


def normalize_app(arg: str) -> str:
	# allow passing "./usertest_mpsc" or "usertest_mpsc"
	p = Path(arg)
	return p.name if p.name else arg


def run_one(app: str, *, timeout_sec: int, verbose: bool) -> tuple[int, str]:
	root = repo_root()
	exe = root / app
	if not exe.exists():
		raise FileNotFoundError(f"missing executable: {exe} (run `make usertest`)")

	cmd = [str(exe)]
	env = os.environ.copy()

	if verbose:
		print(f"[run] {' '.join(cmd)}")

	p = subprocess.run(
		cmd,
		cwd=str(root),
		env=env,
		stdout=subprocess.PIPE,
		stderr=subprocess.STDOUT,
		text=True,
		timeout=timeout_sec,
	)
	return p.returncode, p.stdout


def main(argv: list[str]) -> int:
	parser = argparse.ArgumentParser(description="Run all userspace pthread usertests and validate output.")
	parser.add_argument("apps", nargs="*", help="Optional app names (e.g. usertest_mpsc usertest_vyukhov)")
	parser.add_argument("--list", action="store_true", help="List detected usertest apps and exit")
	parser.add_argument("--build", action="store_true", help="Run `make usertest` before executing")
	parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_SEC, help="Per-test timeout seconds")
	parser.add_argument("--keep-going", action="store_true", help="Continue running remaining tests after a failure")
	parser.add_argument("--quiet", action="store_true", help="Suppress test program output (only print pass/fail)")
	parser.add_argument("-v", "--verbose", action="store_true", help="Verbose runner output (prints commands, extra details)")
	args = parser.parse_args(argv)

	apps = [normalize_app(a) for a in (args.apps or default_apps())]

	if args.list:
		for a in apps:
			print(a)
		return 0

	root = repo_root()
	if args.build:
		r = subprocess.run(["make", "usertest", "-j"], cwd=str(root))
		if r.returncode != 0:
			return r.returncode

	failures: list[str] = []

	for app in apps:
		try:
			rc, out = run_one(app, timeout_sec=args.timeout, verbose=args.verbose)
		except (FileNotFoundError, subprocess.TimeoutExpired) as e:
			failures.append(f"{app}: {e}")
			if not args.keep_going:
				break
			continue

		if not args.quiet:
			sys.stdout.write(f"\n===== {app} (rc={rc}) =====\n")
			sys.stdout.write(out)
			if not out.endswith("\n"):
				sys.stdout.write("\n")

		parsed = parse_output(out)

		ok = True

		if rc != 0:
			ok = False

		if parsed.error_markers:
			ok = False

		if parsed.done_produced is None or parsed.done_consumed is None:
			ok = False
		else:
			if parsed.done_produced != parsed.done_consumed:
				ok = False

		# Validate produced/consumed KV pairs when the runner printed them.
		if parsed.produced_pairs:
			if not parsed.consumed_pairs:
				ok = False
			elif not multiset_eq(parsed.produced_pairs, parsed.consumed_pairs):
				ok = False

		if not ok:
			failures.append(app)
			sys.stdout.write(f"[{app}] FAILED (rc={rc})\n")
			if not args.keep_going:
				break
		else:
			sys.stdout.write(f"[{app}] ok\n")

	if failures:
		sys.stderr.write("\nFailures:\n")
		for f in failures:
			sys.stderr.write(f"  - {f}\n")
		return 1

	return 0


if __name__ == "__main__":
	raise SystemExit(main(sys.argv[1:]))
