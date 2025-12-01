# Quick Start Guide - BPF Arena Data Structures Framework

**New to this framework? Start here!**

## What Does This Do?

This framework lets you test **concurrent data structures** (like lists, trees, queues) where operations happen from:
- **Multiple userspace threads** (via pthreads)
- **Kernel-space BPF programs** (via syscall tracepoints)

Both access the **same memory** (BPF arena) without copying data. Think of it as a shared whiteboard where both kernel and userspace can write simultaneously.

## Prerequisites Check (2 minutes)

```bash
# 1. Check kernel version (need 6.10+)
uname -r

# 2. Check if BPF arena is enabled
zgrep CONFIG_BPF_ARENA /proc/config.gz
# Should show: CONFIG_BPF_ARENA=y

# 3. Check clang version (need 15+)
clang --version

# If any of these fail, see GUIDE.md section "Prerequisites"
```

## Build and Run (5 minutes)

```bash
# Navigate to the framework directory
cd bpf_arena/

# Build everything (takes ~30 seconds)
make -f Makefile.new

# Run a simple test (needs sudo for BPF)
sudo ./skeleton -t 4 -o 1000

# You should see output like:
#   Loading BPF program...
#   BPF programs attached successfully
#   Test Configuration: ...
#   Starting 4 worker threads...
#   All threads completed
#   ============================================================
#                       TEST STATISTICS
#   ============================================================
#   ...
```

## What Just Happened?

1. **Compiled BPF program** → Runs in kernel, triggers on syscalls
2. **Compiled userspace program** → Creates 4 threads
3. **Created shared arena** → Both sides access same memory
4. **Ran 4000 operations** → 4 threads × 1000 ops each
5. **Printed statistics** → Throughput, failures, memory usage

## Try Different Tests

```bash
# More threads (8 instead of 4)
sudo ./skeleton -t 8 -o 1000

# Different workload (only insertions)
sudo ./skeleton -t 4 -o 1000 -w insert

# Verify data structure integrity
sudo ./skeleton -t 4 -o 1000 -v

# Run all smoke tests
sudo ./test_smoke.sh
```

## Understanding the Output

```
Per-Thread Results:
Thread   Operations    Inserts    Deletes   Searches  Failures      Ops/sec
--------------------------------------------------------------------
0              1000        500        200        300         5       45231
```

- **Thread**: Thread ID (0, 1, 2, 3...)
- **Operations**: Total ops performed by this thread
- **Inserts/Deletes/Searches**: Breakdown by operation type
- **Failures**: Operations that failed (e.g., delete non-existent key) - this is NORMAL
- **Ops/sec**: Throughput (higher is better)

```
Arena Memory Statistics:
  Allocations:      2144
  Frees:            801
  Current allocs:   1343
```

- **Allocations**: Total memory allocations from arena
- **Frees**: Total deallocations
- **Current allocs**: Active allocations (should ≈ final element count)
- **Failed allocs**: Should be 0 (or something's wrong)

## What's in the Box?

### Core Files (You'll Use These)

```
libarena_ds.h     - Memory allocator for arena
ds_api.h          - Template for new data structures
ds_list.h         - Example: linked list implementation
skeleton.bpf.c    - Kernel-side test program
skeleton.c        - Userspace test program
Makefile.new      - Build system
```

### Documentation

```
GUIDE.md                - Complete guide (read this next!)
README_FRAMEWORK.md     - Framework overview
QUICKSTART.md          - This file
```

### Testing Scripts

```
test_smoke.sh     - Quick tests (~30 seconds)
test_stress.sh    - Heavy tests (~5 minutes)
test_verify.sh    - Correctness checks
benchmark.sh      - Performance measurement
```

## Next Steps

### 1. Run All Tests (10 minutes)

```bash
# Quick smoke tests
sudo ./test_smoke.sh

# If those pass, try stress tests
sudo ./test_stress.sh

# Check correctness
sudo ./test_verify.sh
```

### 2. Read the Full Guide (30 minutes)

```bash
# Open the comprehensive guide
less GUIDE.md

# Or in your browser/editor
# Key sections:
#   - Architecture (how it works)
#   - Adding New Data Structures (step-by-step)
#   - Troubleshooting (when things go wrong)
```

### 3. Add Your Own Data Structure (1-2 hours)

Follow the guide in `GUIDE.md` → "Adding New Data Structures"

Example: Add a binary search tree
- Create `ds_tree.h`
- Modify `skeleton.bpf.c` (3 places marked with `/* DS_API_INSERT */`)
- Modify `skeleton.c` (update types and functions)
- Build and test!

## Common Issues

### "Failed to load BPF program"

**Cause**: Kernel doesn't support BPF arena, or missing BTF

**Fix**: 
```bash
# Check kernel config
zgrep CONFIG_BPF_ARENA /proc/config.gz

# If not enabled, you need kernel 6.10+ with arena support
```

### "Permission denied"

**Cause**: BPF operations need root privileges

**Fix**: Add `sudo`
```bash
sudo ./skeleton -t 4 -o 1000
```

### Compilation errors

**Cause**: Missing dependencies or wrong paths

**Fix**:
```bash
# Check if vmlinux.h exists
ls ../../third_party/vmlinux/$(uname -m)/vmlinux.h

# Check if libbpf source exists
ls ../../third_party/libbpf/src/

# If missing, adjust paths in Makefile.new
```

### Program crashes or hangs

**Cause**: Various (see GUIDE.md troubleshooting section)

**Quick check**:
```bash
# Run with just 1 thread to isolate
sudo ./skeleton -t 1 -o 100

# Check dmesg for kernel messages
sudo dmesg | tail -50

# Enable verbose build to see compiler output
make -f Makefile.new clean && make -f Makefile.new V=1
```

## File Naming Conventions

When you add new data structures, follow these patterns:

```
Data structure header:   ds_<name>.h       (e.g., ds_tree.h)
BPF kernel program:      <name>.bpf.c      (e.g., tree_test.bpf.c)
Userspace program:       <name>.c          (e.g., tree_test.c)

Functions:               ds_<name>_<op>    (e.g., ds_tree_insert)
```

## Getting Help

1. **Read GUIDE.md** - Most questions answered there
2. **Check existing code** - `ds_list.h` is a complete example
3. **Look for markers** - `/* DS_API_INSERT */` shows where to modify
4. **Run tests** - `test_smoke.sh` catches common issues
5. **Check dmesg** - Kernel logs show BPF errors

## Testing Workflow

```bash
# 1. Make changes to your data structure
vim ds_mylist.h

# 2. Update skeleton files (follow markers)
vim skeleton.bpf.c  # Add includes and dispatch
vim skeleton.c      # Update types and calls

# 3. Build
make -f Makefile.new clean && make -f Makefile.new

# 4. Quick test
sudo ./skeleton -t 1 -o 100

# 5. Full test
sudo ./skeleton -t 4 -o 1000 -v

# 6. Automated tests
sudo ./test_smoke.sh
```

## Cheat Sheet

```bash
# Build
make -f Makefile.new                    # Build all
make -f Makefile.new skeleton           # Build just skeleton
make -f Makefile.new clean              # Clean all

# Run
sudo ./skeleton -t T -o O -w W          # T threads, O ops, W workload
sudo ./skeleton -t 4 -o 1000 -v         # With verification
sudo ./skeleton -t 8 -o 5000 -w insert  # Insert-only

# Test
sudo ./test_smoke.sh                    # Quick tests
sudo ./test_stress.sh                   # Stress tests
sudo ./test_verify.sh                   # Verify correctness
sudo ./benchmark.sh                     # Measure performance

# Workload types
-w insert     # Only insertions
-w search     # Only searches
-w delete     # Only deletions
-w mixed      # Mix of all (default)
```

## Success Checklist

- [ ] Kernel 6.10+ with `CONFIG_BPF_ARENA=y`
- [ ] Clang 15+ installed
- [ ] Built successfully with `make -f Makefile.new`
- [ ] Ran `sudo ./skeleton -t 4 -o 1000` without errors
- [ ] `test_smoke.sh` passes all tests
- [ ] Read GUIDE.md sections you need

**All checked? You're ready to add your own data structures!**

---

**Still stuck?** Read the comprehensive guide:
```bash
less GUIDE.md
# Or
cat GUIDE.md | grep -A 20 "Troubleshooting"
```
