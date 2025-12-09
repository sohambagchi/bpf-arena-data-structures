# Quick Start Guide - BPF Arena Data Structures Framework

**New to this framework? Start here!**

## What Does This Do?

This framework lets you test **data structures in shared memory** (BPF arena) where:
- **Kernel-space BPF programs** insert data automatically (via LSM hooks triggered on file creation)
- **Userspace programs** read the data structure directly (single-threaded, after a sleep period)

Both access the **same memory** (BPF arena) without copying data. The kernel populates the data structure automatically when files are created, and userspace can read it directly after sleeping.

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
sudo ./skeleton -d 5

# You should see output like:
#   Loading BPF program...
#   BPF programs attached successfully
#   Sleeping for 5 seconds to allow kernel to populate data structure...
#   Reading data structure...
#   Element 0: pid=1234, last_ts=1234567890
#   ...
#   Total elements in list: 42
#   ============================================================
#                       STATISTICS
#   ============================================================
```

## What Just Happened?

1. **Compiled BPF program** → Runs in kernel, attached to LSM hook (inode_create)
2. **Compiled userspace program** → Single-threaded reader
3. **Created shared arena** → Both sides access same memory (BPF arena)
4. **Kernel inserts data** → LSM hook triggers automatically on file creations
5. **Userspace sleeps 5 seconds** → Allows kernel time to populate data structure
6. **Userspace reads results** → Direct arena access, no syscalls needed
7. **Printed statistics** → Element count, insert statistics

## Try Different Tests

```bash
# Sleep longer to collect more data
sudo ./skeleton -d 30

# Verify data structure integrity
sudo ./skeleton -d 5 -v

# Show statistics (enabled by default)
sudo ./skeleton -d 5 -s

# Test the Michael-Scott queue
sudo ./skeleton_msqueue -d 10

# Test MS queue with verification
sudo ./skeleton_msqueue -d 5 -v
```

## Understanding the Output

```
Reading data structure...
  Element 0: pid=1234, last_ts=1234567890
  Element 1: pid=1235, last_ts=1234567891
  ...
Total elements in list: 42

============================================================
                    STATISTICS
============================================================

Kernel-Side Operations (inode_create LSM hook inserts):
  Total inserts:    42
  Insert failures:  0

Data Structure State:
  Elements in list: 42
```

- **pid**: Process ID that triggered the LSM hook
- **last_ts**: Timestamp when the insertion occurred
- **Total inserts**: Number of times the LSM hook fired and inserted data
- **Insert failures**: Should be 0 (would indicate memory allocation issues)
- **Elements in list**: Final count from data structure (should match total inserts)

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
# Run with short duration
sudo ./skeleton -d 1

# Check dmesg for kernel messages
sudo dmesg | tail -50

# Enable verbose build to see compiler output
make clean && make V=1
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
vim ds_myds.h

# 2. Update skeleton files (follow markers with /* DS_API_INSERT */)
vim skeleton.bpf.c  # Add includes and dispatch
vim skeleton.c      # Update includes and types

# 3. Build
make clean && make

# 4. Quick test (short sleep)
sudo ./skeleton -d 2

# 5. Full test (longer sleep with verification)
sudo ./skeleton -d 10 -v

# 6. Check statistics
sudo ./skeleton -d 5 -s
```

## Cheat Sheet

```bash
# Build
make                                    # Build all
make skeleton                           # Build just skeleton (list)
make skeleton_msqueue                   # Build just MS queue
make clean                              # Clean all

# Run (list)
sudo ./skeleton -d N                    # Sleep N seconds, then read
sudo ./skeleton -d 5 -v                 # With verification
sudo ./skeleton -d 10 -s                # With statistics

# Run (MS queue)
sudo ./skeleton_msqueue -d N            # Sleep N seconds, then read
sudo ./skeleton_msqueue -d 5 -v         # With verification

# Options
-d N      # Sleep duration in seconds (default: 5)
-v        # Verify data structure integrity
-s        # Print statistics (default: enabled)
-h        # Show help
```

## Success Checklist

- [ ] Kernel 6.10+ with `CONFIG_BPF_ARENA=y`
- [ ] Clang 15+ installed
- [ ] Built successfully with `make`
- [ ] Ran `sudo ./skeleton -d 5` without errors
- [ ] Ran `sudo ./skeleton_msqueue -d 5` without errors
- [ ] Data structure output shows elements
- [ ] Read GUIDE.md sections you need

**All checked? You're ready to add your own data structures!**

---

**Still stuck?** Read the comprehensive guide:
```bash
less GUIDE.md
# Or
cat GUIDE.md | grep -A 20 "Troubleshooting"
```
