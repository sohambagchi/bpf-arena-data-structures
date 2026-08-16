#!/usr/bin/env python3
"""Check that a kernel .config can build and run the arena relay apps.

Text analysis of a .config, plus -- when it is the config the running kernel
booted from -- a runtime check of the LSM list, because an `lsm=` command line
replaces CONFIG_LSM outright and only /sys/kernel/security/lsm knows what was
actually activated.

Usage:
    python3 scripts/check_kconfig.py [path/to/.config]
    python3 scripts/check_kconfig.py --print-lsm-fix   # emit the lsm= line to set

Defaults to /boot/config-$(uname -r), falling back to /proc/config.gz.

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

RUNTIME_LSM_PATH = "/sys/kernel/security/lsm"
CMDLINE_PATH = "/proc/cmdline"

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

MUST_BE_OFF = [
    ("DEBUG_INFO_SPLIT", "DEBUG_INFO_BTF depends on !DEBUG_INFO_SPLIT"),
    ("DEBUG_INFO_REDUCED", "DEBUG_INFO_BTF depends on !DEBUG_INFO_REDUCED"),
]

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


def is_running_config(path):
    """True if @path is the config the running kernel booted from.

    Runtime probing is only meaningful then. build-kernel.sh checks a .config
    inside a kernel tree, and `nix flake check` checks the reference config in
    a sandbox; neither has anything to do with /sys/kernel/security/lsm.
    """
    default = default_config()
    if not default:
        return False
    try:
        return os.path.realpath(path) == os.path.realpath(default)
    except OSError:
        return False


def split_lsm(value):
    """Split an LSM list ("a,b,c") into entries, tolerating quotes and spaces."""
    return [e.strip() for e in value.strip().strip('"').split(",") if e.strip()]


def read_active_lsms():
    """LSMs the running kernel actually activated, or None if unreadable.

    Unreadable usually means securityfs is not mounted, which is not itself a
    failure of the kernel config.
    """
    try:
        with open(RUNTIME_LSM_PATH, errors="replace") as fh:
            return split_lsm(fh.read())
    except OSError:
        return None


def read_cmdline_lsm():
    """The lsm= override from /proc/cmdline, or None if there is not one."""
    try:
        with open(CMDLINE_PATH, errors="replace") as fh:
            cmdline = fh.read()
    except OSError:
        return None
    for token in cmdline.split():
        if token.startswith("lsm="):
            return split_lsm(token[len("lsm="):])
    return None


def lsm_fix_value(config_lsm, cmdline_lsm):
    """The lsm= value to set: whichever list is in effect, plus bpf.

    Never a canned list. Because lsm= replaces rather than extends, emitting a
    fixed string would silently disable whatever it omits -- AppArmor on
    Ubuntu, SELinux on Fedora. Start from the list actually in effect: the
    command line if one is pinned, otherwise the config's.
    """
    base = cmdline_lsm if cmdline_lsm is not None else (config_lsm or [])
    return ",".join(base + ["bpf"]) if "bpf" not in base else ",".join(base)


def print_lsm_fix(value, indent="  "):
    """Print how to apply an lsm= value. Prints only -- never edits anything."""
    pad = indent
    print("%sAdd to the kernel command line (it replaces the built-in list, so" % pad)
    print("%sthis one already includes everything active now), then reboot:" % pad)
    print()
    print("%s    lsm=%s" % (pad, value))
    print()
    print("%sOn Debian/Ubuntu: GRUB_CMDLINE_LINUX in /etc/default/grub, then" % pad)
    print("%ssudo update-grub." % pad)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", nargs="?", help="path to a .config (or config.gz)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="print only failures")
    ap.add_argument("--runtime", dest="runtime", action="store_true", default=None,
                    help="force the runtime LSM check on")
    ap.add_argument("--no-runtime", dest="runtime", action="store_false",
                    help="skip the runtime LSM check (config text only)")
    ap.add_argument("--print-lsm-fix", action="store_true",
                    help="print the lsm= line this machine needs, and exit")
    args = ap.parse_args()

    if args.print_lsm_fix:
        path = args.config or default_config()
        config_lsms = split_lsm(load(path)[0].get("LSM", "")) if path else []
        active = read_active_lsms()
        if active is not None and "bpf" in active:
            print("The BPF LSM is already active (%s: %s); nothing to set."
                  % (RUNTIME_LSM_PATH, ",".join(active)))
            return 0
        print_lsm_fix(lsm_fix_value(config_lsms, read_cmdline_lsm()), indent="")
        return 0

    path = args.config or default_config()
    if not path:
        sys.exit("error: no config given and none found at "
                 "/boot/config-$(uname -r) or /proc/config.gz")
    if not os.path.exists(path):
        sys.exit("error: no such file: %s" % path)

    values, unset = load(path)
    if not values:
        sys.exit("error: %s contains no CONFIG_ lines" % path)

    runtime = args.runtime if args.runtime is not None else is_running_config(path)

    print("Checking %s (%d set, %d unset)%s\n"
          % (path, len(values), len(unset),
             "" if runtime else "  [config text only]"))

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

    lsm = values.get("LSM", "").strip('"')
    config_lsms = split_lsm(lsm)
    config_lsm_ok = "bpf" in config_lsms

    active = read_active_lsms() if runtime else None
    cmdline_lsms = read_cmdline_lsm() if runtime else None
    lsm_fix = None

    if active is None:
        lsm_ok = config_lsm_ok
        if not lsm_ok:
            failures.append(("LSM", '="%s"' % lsm, True,
                             'must list "bpf" or lsm.s programs cannot attach'))
    else:
        lsm_ok = "bpf" in active
        if not lsm_ok:
            lsm_fix = lsm_fix_value(config_lsms, cmdline_lsms)
            failures.append(("LSM", "not active at runtime", None,
                             'the running kernel did not activate the BPF LSM'))

    if not args.quiet or not lsm_ok:
        print("  LSM ordering")
        config_mark = "ok" if config_lsm_ok else ("--" if lsm_ok else "!!")
        print("    [%s] CONFIG_LSM contains \"bpf\"%s"
              % (config_mark,
                 "" if config_lsm_ok else "  (found: %r)" % lsm))
        if active is not None:
            print("    [%s] %s contains \"bpf\"  (active: %s)"
                  % ("ok" if lsm_ok else "!!", RUNTIME_LSM_PATH, ",".join(active)))
            if cmdline_lsms is not None:
                print("         lsm=%s on the kernel command line overrides "
                      "CONFIG_LSM" % ",".join(cmdline_lsms))
            if lsm_ok and not config_lsm_ok:
                print("         (the command line supplies what the config "
                      "does not -- this works)")
        elif runtime:
            print("    [--] %s unreadable; runtime LSM state unverified"
                  % RUNTIME_LSM_PATH)
            print("         (securityfs not mounted? "
                  "sudo mount -t securityfs none /sys/kernel/security)")
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
        frag = [f for f in failures if f[2] is True]
        derived = [f for f in failures if f[2] is False]
        if frag:
            print("  Set via kernel/configs/delta-bpf-arena.config:")
            for sym, state, _, _ in frag:
                print("    CONFIG_%s (%s)" % (sym, state))
        if derived:
            print("\n  Derived symbols that did not come out as expected --")
            print("  these are select'ed/def_bool, so chase their dependencies")
            print("  rather than editing them directly:")
            for sym, state, _, why in derived:
                print("    CONFIG_%s (%s) -- %s" % (sym, state, why))
        if lsm_fix:
            print("\n  The BPF LSM is compiled in but was not activated at boot.")
            print("  This needs no kernel rebuild -- only a command line change:\n")
            print_lsm_fix(lsm_fix)
        return 1

    print("OK: all requirements met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
