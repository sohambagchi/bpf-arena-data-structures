#!/usr/bin/env python3
"""Check that a kernel .config can build and run the arena relay apps.

Pure text analysis -- this never builds a kernel or runs an experiment.

Usage:
    python3 scripts/check_kconfig.py [path/to/.config]

Defaults to /boot/config-$(uname -r), falling back to /proc/config.gz.

The companion file kernel/configs/bpf-arena.config sets only the
user-settable symbols. This script additionally verifies the derived ones
(def_bool / select-only) that olddefconfig is expected to turn on, since a
fragment cannot set those and a silently-missing one shows up much later as
a confusing load or attach failure.

Exit status: 0 if every requirement is met, 1 otherwise.
"""

import argparse
import gzip
import os
import platform
import re
import sys

SET_RE = re.compile(r"^CONFIG_([A-Z0-9_]+)=(.*)$")
UNSET_RE = re.compile(r"^# CONFIG_([A-Z0-9_]+) is not set$")

# (symbol, group, settable_by_fragment, why)
REQUIRED = [
    ("BPF_SYSCALL", "arena", True,
     "bpf() syscall; kernel/bpf/Makefile gates arena.o on it"),
    ("BPF", "arena", False, "eBPF core, select'ed by BPF_SYSCALL"),
    ("64BIT", "arena", False, "arena.o builds only under MMU && 64BIT"),
    ("MMU", "arena", False, "same gate as 64BIT"),
    ("BPF_JIT", "arena", True,
     "arena addr_space_cast and BPF trampolines need the JIT"),
    ("HAVE_EBPF_JIT", "arena", False, "arch provides an eBPF JIT"),
    ("BPF_JIT_DEFAULT_ON", "arena", False,
     "JIT active at boot without tuning net.core.bpf_jit_enable"),

    ("BPF_LSM", "lsm", True, 'the SEC("lsm.s/inode_create") program type'),
    ("SECURITY", "lsm", True, "hard dependency of BPF_LSM"),
    ("BPF_EVENTS", "lsm", False,
     "hard dependency of BPF_LSM; also gates the tracing kfuncs"),

    ("PERF_EVENTS", "uprobe", True, "uprobe links are perf events"),
    ("KPROBES", "uprobe", True, "probe-event machinery"),
    ("KPROBE_EVENTS", "uprobe", True, "probe-event machinery"),
    ("UPROBE_EVENTS", "uprobe", True,
     "attach path used by bpf_program__attach_uprobe_opts()"),
    ("UPROBES", "uprobe", False, "userspace probe core, select'ed by UPROBE_EVENTS"),

    ("FTRACE", "trampoline", True, "tracing core; also trace_pipe for bpf_printk()"),
    ("FUNCTION_TRACER", "trampoline", True, "patchsites for trampoline attach"),
    ("DYNAMIC_FTRACE", "trampoline", True, "runtime patching of those sites"),
    ("DYNAMIC_FTRACE_WITH_DIRECT_CALLS", "trampoline", False,
     "register_ftrace_direct(), used by BPF trampolines"),
    ("DYNAMIC_FTRACE_WITH_REGS", "trampoline", False, "trampoline arg access"),

    ("DEBUG_INFO_BTF", "btf", True,
     "exports /sys/kernel/btf/vmlinux; required to attach lsm.s and uprobe.s"),
    ("DEBUG_INFO", "btf", False,
     "prerequisite for BTF; comes from any non-None DWARF choice"),
]

GROUP_TITLES = {
    "arena": "BPF arena (BPF_MAP_TYPE_ARENA)",
    "lsm": 'BPF LSM producer -- SEC("lsm.s/inode_create")',
    "uprobe": 'uprobe consumer -- SEC("uprobe.s")',
    "trampoline": "BPF trampoline (fentry-style attach)",
    "btf": "BTF / CO-RE",
}

# Required to be off, because DEBUG_INFO_BTF depends on !X.
MUST_BE_OFF = [
    ("DEBUG_INFO_SPLIT", "DEBUG_INFO_BTF depends on !DEBUG_INFO_SPLIT"),
    ("DEBUG_INFO_REDUCED", "DEBUG_INFO_BTF depends on !DEBUG_INFO_REDUCED"),
]

# (symbol, note) -- reported but never fatal.
OPTIONAL = [
    ("DEBUG_INFO_BTF_MODULES", "BTF for kernel modules"),
    ("IO_URING", "only needed for a real-io_uring comparison variant; "
                 "src/skeleton_io_uring.c does not use the syscall"),
    ("KCOV", "only needed for a real-kcov comparison variant; "
             "src/skeleton_kcov.c does not open /sys/kernel/debug/kcov"),
]


def load(path):
    """Return (values, unset) parsed from a .config, plain or gzipped."""
    opener = gzip.open if path.endswith(".gz") else open
    values, unset = {}, set()
    with opener(path, "rt", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = SET_RE.match(line)
            if m:
                values[m.group(1)] = m.group(2)
                continue
            m = UNSET_RE.match(line)
            if m:
                unset.add(m.group(1))
    return values, unset


def default_config():
    for cand in ("/boot/config-" + platform.release(), "/proc/config.gz"):
        if os.path.exists(cand):
            return cand
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", nargs="?", help="path to a .config (or config.gz)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="print only failures")
    args = ap.parse_args()

    path = args.config or default_config()
    if not path:
        sys.exit("error: no config given and none found at "
                 "/boot/config-$(uname -r) or /proc/config.gz")
    if not os.path.exists(path):
        sys.exit("error: no such file: %s" % path)

    values, unset = load(path)
    if not values:
        sys.exit("error: %s contains no CONFIG_ lines" % path)

    print("Checking %s (%d set, %d unset)\n" % (path, len(values), len(unset)))

    failures = []
    current_group = None
    for sym, group, settable, why in REQUIRED:
        if group != current_group:
            if not args.quiet:
                print("  %s" % GROUP_TITLES[group])
            current_group = group
        ok = values.get(sym) == "y"
        if not ok:
            state = "not set" if sym in unset else \
                    ("=%s" % values[sym] if sym in values else "absent")
            failures.append((sym, state, settable, why))
        if not args.quiet or not ok:
            print("    [%s] CONFIG_%-38s %s" % ("ok" if ok else "!!", sym, why))
    if not args.quiet:
        print()

    # CONFIG_LSM is a string, not a bool: it must list "bpf".
    lsm = values.get("LSM", "").strip('"')
    lsm_ok = "bpf" in [e.strip() for e in lsm.split(",")]
    if not lsm_ok:
        failures.append(("LSM", '="%s"' % lsm, True,
                         'must list "bpf" or lsm.s programs cannot attach'))
    if not args.quiet or not lsm_ok:
        print("  LSM ordering")
        print("    [%s] CONFIG_LSM contains \"bpf\"%s"
              % ("ok" if lsm_ok else "!!",
                 "" if lsm_ok else "  (found: %r)" % lsm))
        print()

    for sym, why in MUST_BE_OFF:
        off = values.get(sym) != "y"
        if not off:
            failures.append((sym, "=y but must be off", True, why))
            print("    [!!] CONFIG_%-38s %s" % (sym, why))

    if not args.quiet:
        print("  Optional")
        for sym, note in OPTIONAL:
            print("    [%s] CONFIG_%-38s %s"
                  % ("on" if values.get(sym) == "y" else "--", sym, note))
        print()

    warnings = []
    if values.get("KCOV") == "y" and values.get("KCOV_INSTRUMENT_ALL") == "y":
        warnings.append(
            "KCOV_INSTRUMENT_ALL=y instruments every kernel function; it will "
            "distort the ds_metrics latency/throughput numbers. Set "
            "'# CONFIG_KCOV_INSTRUMENT_ALL is not set' for benchmark kernels.")
    if values.get("BPF_JIT_ALWAYS_ON") == "y":
        warnings.append(
            "BPF_JIT_ALWAYS_ON=y removes the interpreter. Fine here (the JIT "
            "is required anyway), just noting it is not the reference build.")
    for w in warnings:
        print("  warning: %s\n" % w)

    if failures:
        print("FAIL: %d requirement(s) unmet\n" % len(failures))
        frag = [f for f in failures if f[2]]
        derived = [f for f in failures if not f[2]]
        if frag:
            print("  Set via kernel/configs/bpf-arena.config:")
            for sym, state, _, _ in frag:
                print("    CONFIG_%s (%s)" % (sym, state))
        if derived:
            print("\n  Derived symbols that did not come out as expected --")
            print("  these are select'ed/def_bool, so chase their dependencies")
            print("  rather than editing them directly:")
            for sym, state, _, why in derived:
                print("    CONFIG_%s (%s) -- %s" % (sym, state, why))
        return 1

    print("OK: all requirements met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
