# BPF Arena Concurrent Data Structures Testing Framework

A comprehensive framework for testing concurrent data structures using **BPF arena** - enabling operations from both kernel-space (BPF programs) and user-space (direct arena access) with zero-copy shared memory.

## 🎯 Quick Start

```bash
# Build everything
make -f Makefile.new

# Run basic test (4 threads, 1000 operations)
sudo ./skeleton -t 4 -o 1000

# Run all smoke tests
sudo ./test_smoke.sh

# See comprehensive guide
cat GUIDE.md
```

## 📋 What's Included

### Core Library Components

- **`libarena_ds.h`** - Arena memory allocator with statistics tracking and atomic operations
- **`ds_api.h`** - Standard API template for data structure implementations
- **`bpf_arena_common.h`** - Common definitions for BPF/userspace compatibility
- **`bpf_experimental.h`** - BPF experimental features

### Test Framework

- **`skeleton.bpf.c`** - Kernel-side BPF program with syscall tracepoints
- **`skeleton.c`** - Userspace test driver with pthread workers
- **`ds_list.h`** - Reference implementation (doubly-linked list)

### Testing Infrastructure

- **`test_smoke.sh`** - Quick smoke tests (5 tests, ~30 seconds)
- **`test_stress.sh`** - Stress tests (5 tests, ~5 minutes)
- **`test_verify.sh`** - Correctness verification (6 tests)
- **`benchmark.sh`** - Performance benchmarking

### Documentation

- **`GUIDE.md`** - Comprehensive guide (architecture, adding data structures, conventions)
- **`README_FRAMEWORK.md`** - This file

### Build System

- **`Makefile.new`** - Annotated Makefile with all build rules

## 🏗️ Architecture

```
┌─────────────────────────────────────────┐
│         USER SPACE                      │
│  Multiple threads via pthreads          │
│  Direct arena access (no syscalls)      │
├─────────────────────────────────────────┤
│         BPF ARENA                       │
│  Shared memory region (up to 4GB)       │
│  Accessible from both contexts          │
├─────────────────────────────────────────┤
│         KERNEL SPACE                    │
│  BPF programs via syscall tracepoints   │
│  Operations triggered by exec/exit      │
└─────────────────────────────────────────┘
```

## 🚀 Usage Examples

### Basic Testing

```bash
# 4 threads, 1000 operations each, mixed workload
sudo ./skeleton -t 4 -o 1000 -w mixed

# 8 threads, insert-only workload
sudo ./skeleton -t 8 -o 5000 -w insert

# With verification
sudo ./skeleton -t 4 -o 1000 -v
```

### Automated Testing

```bash
# Quick smoke tests
sudo ./test_smoke.sh

# Stress tests (takes longer)
sudo ./test_stress.sh

# Correctness verification
sudo ./test_verify.sh

# Performance benchmarking
sudo ./benchmark.sh
```

### Command-Line Options

```
./skeleton [OPTIONS]

  -t N    Number of userspace threads (default: 4)
  -o N    Operations per thread (default: 1000)
  -k N    Key range for operations (default: 10000)
  -w TYPE Workload: insert, search, delete, mixed (default: mixed)
  -v      Verify data structure integrity
  -s      Print statistics
  -K      Trigger kernel operations via syscalls
  -h      Show help
```

## 📦 Requirements

### System Requirements

- **Linux kernel 6.10+** with `CONFIG_BPF_ARENA=y`
- **LLVM/Clang 15+** with BPF support
- **libbpf 1.0+**
- **libelf, zlib**

### Check Your System

```bash
# Kernel version
uname -r

# Arena support
zgrep CONFIG_BPF_ARENA /proc/config.gz

# Clang version
clang --version
```

## 🔧 Adding New Data Structures

### 1. Create Data Structure Header (`ds_<name>.h`)

```c
#pragma once
#include "ds_api.h"

struct ds_<name>_node {
    // Your node fields
    __u64 key;
    __u64 value;
};

struct ds_<name>_head {
    struct ds_<name>_node __arena *root;
    __u64 count;
    struct ds_stats stats;
};

// Implement required operations:
// - ds_<name>_init()
// - ds_<name>_insert()
// - ds_<name>_delete()
// - ds_<name>_search()
// - ds_<name>_verify()
// - ds_<name>_get_stats()
// - ds_<name>_reset_stats()
// - ds_<name>_get_metadata()
```

### 2. Modify `skeleton.bpf.c`

Look for `/* DS_API_INSERT */` markers and add:
- Include your header
- Declare your data structure head
- Update operation dispatch in `handle_operation()`

### 3. Modify `skeleton.c`

- Include your header
- Update worker context type
- Update workload functions to use your API

### 4. Build and Test

```bash
make -f Makefile.new clean && make -f Makefile.new
sudo ./skeleton -t 4 -o 1000 -v
```

**See `GUIDE.md` for detailed step-by-step instructions.**

## 📊 Understanding Test Output

```
============================================================
                    TEST STATISTICS                         
============================================================

Per-Thread Results:
Thread   Operations    Inserts    Deletes   Searches  Failures      Ops/sec
--------------------------------------------------------------------
0              1000        500        200        300         5       45231
1              1000        498        202        300         8       44892
...
--------------------------------------------------------------------
TOTAL          4000       1999        801       1200        22      180813

Kernel-Side Operations:
  Total operations: 145
  Total failures:   3

Arena Memory Statistics:
  Allocations:      2144
  Frees:            801
  Current allocs:   1343
  Failed allocs:    0
```

### Key Metrics

- **Ops/sec**: Throughput (higher is better)
- **Failures**: Expected for operations like delete/search on non-existent keys
- **Current allocs**: Should approximately match final element count
- **Failed allocs**: Should be 0 or very low

## 📁 File Structure

```
bpf_arena/
├── libarena_ds.h           # Arena allocation library ⭐
├── ds_api.h                # API template ⭐
├── ds_list.h               # Example: linked list ⭐
├── skeleton.bpf.c          # Kernel-side driver ⭐
├── skeleton.c              # Userspace driver ⭐
├── Makefile.new            # Build system ⭐
├── GUIDE.md                # Comprehensive guide ⭐
├── README_FRAMEWORK.md     # This file
├── test_smoke.sh           # Smoke tests
├── test_stress.sh          # Stress tests
├── test_verify.sh          # Verification tests
├── benchmark.sh            # Performance benchmarks
├── bpf_arena_common.h      # Common definitions
├── bpf_arena_alloc.h       # Original allocator (reference)
├── bpf_arena_list.h        # Original list (reference)
├── arena_list.bpf.c        # Original example
└── arena_list.c            # Original example
```

⭐ = Core framework files

## 🎓 Nomenclature

### File Naming
- Data structures: `ds_<name>.h` (e.g., `ds_tree.h`)
- BPF programs: `<name>.bpf.c`
- Userspace: `<name>.c`

### Function Naming
```c
ds_<name>_<operation>

Examples:
- ds_list_insert()
- ds_tree_search()
- ds_queue_delete()
```

### Required Operations
Every data structure must implement:
- `init` - Initialize structure
- `insert` - Add element
- `delete` - Remove element
- `search` - Find element
- `verify` - Check integrity
- `get_stats` - Get statistics
- `reset_stats` - Reset statistics
- `get_metadata` - Get metadata

## 🐛 Troubleshooting

### Compilation Issues

**"vmlinux.h not found"**
```bash
bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h
```

**"CONFIG_BPF_ARENA not enabled"**
- Need kernel 6.10+ with arena support
- Recompile kernel or use a supported distribution

### Runtime Issues

**"Failed to load BPF program"**
- Check verifier logs: `sudo bpftool prog load ...`
- Common: missing loop bounds (`&& can_loop`)
- Common: null pointer checks

**"Permission denied"**
- Must run with `sudo`

**Segmentation fault**
- Arena not properly initialized
- Check skeleton loading succeeded

### Performance Issues

**Low throughput**
- Reduce key range for more contention
- Check if verification is enabled (slower)

**High failure rate**
- Expected for delete/search operations
- Check arena size if allocation failures occur

## 📚 Additional Resources

- **Comprehensive Guide**: See `GUIDE.md` for detailed documentation
- **Linux BPF Arena**: `Documentation/bpf/arena.rst` in kernel source
- **libbpf**: https://libbpf.readthedocs.io/
- **Original Example**: See `arena_list.bpf.c` and `arena_list.c`

## 🤝 For Agents and Automation

This framework is designed to be agent-friendly:

- **Standardized API**: All data structures follow `ds_api.h` template
- **Clear insertion points**: Look for `/* DS_API_INSERT */` markers
- **JSON schema**: See `GUIDE.md` for structured metadata format
- **Testing checklist**: See `GUIDE.md` for automated testing guidance
- **Common patterns**: Reference implementations in `ds_list.h`

## ⚡ Quick Reference

```bash
# Build
make -f Makefile.new

# Test
sudo ./skeleton -t 4 -o 1000 -w mixed -v

# Automated testing
sudo ./test_smoke.sh      # Quick (~30s)
sudo ./test_stress.sh     # Thorough (~5min)
sudo ./test_verify.sh     # Correctness
sudo ./benchmark.sh       # Performance

# Clean
make -f Makefile.new clean
```

## 📝 License

SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

Based on BPF arena examples from the Linux kernel, Copyright (c) 2024 Meta Platforms, Inc. and affiliates.

---

**Ready to test concurrent data structures? Start with `GUIDE.md` for comprehensive documentation!**
