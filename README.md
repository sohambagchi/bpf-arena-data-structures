# BPF Arena Concurrent Data Structures Testing Framework

A comprehensive framework for testing concurrent data structures using **BPF arena** - enabling operations from both kernel-space (BPF programs) and user-space (direct arena access) with zero-copy shared memory.

## 🎯 Quick Start

```bash
# Build everything
make

# Run basic test
sudo ./skeleton -d 5

# Run with verification  
sudo ./skeleton -d 5 -v

# See comprehensive guide
cat GUIDE.md
```

## 📋 What's Included

### Core Library Components

- **`libarena_ds.h`** - Simple bump allocator for arena memory
- **`ds_api.h`** - Standard API template for data structure implementations
- **`bpf_arena_common.h`** - Common definitions for BPF/userspace compatibility
- **`bpf_experimental.h`** - BPF experimental features

### Test Framework

- **`skeleton.bpf.c`** - Kernel-side BPF program with LSM hooks
- **`skeleton.c`** - Userspace reader program
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
│  BPF programs via LSM hooks             │
│  Operations triggered by file creation  │
└─────────────────────────────────────────┘
```

## 🚀 Usage Examples

### Basic Testing

```bash
# Run for 10 seconds, then read results
sudo ./skeleton -d 10

# Run for 5 seconds with data structure verification
sudo ./skeleton -d 5 -v

# Run with statistics output (enabled by default)
sudo ./skeleton -d 5 -s
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

DESIGN:
  Kernel:    LSM hook on inode_create inserts items
  Userspace: Single thread sleeps, then reads data structure

OPTIONS:
  -d N    Sleep duration in seconds before reading (default: 5)
  -v      Verify data structure integrity
  -s      Print statistics (default: enabled)
  -h      Show this help
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
make clean && make
sudo ./skeleton -d 5 -v
```

**See `GUIDE.md` for detailed step-by-step instructions.**

## 📊 Understanding Test Output

```
============================================================
                    STATISTICS                              
============================================================

Kernel-Side Operations (inode_create LSM hook inserts):
  Total inserts:    342
  Insert failures:  0

Data Structure State:
  Elements in list: 342
```

### Key Metrics

- **Total inserts**: Number of items inserted by kernel LSM hook
- **Insert failures**: Should be 0 (or indicates memory issues)
- **Elements in list**: Final count in data structure (should match total inserts)

### How It Works

1. Kernel LSM hook on `inode_create` triggers on file creation
2. Each trigger inserts process ID and timestamp into the list
3. Userspace sleeps for specified duration while kernel populates
4. After sleep, userspace reads and displays the data structure contents

## 📁 File Structure

```
bpf_arena/
├── libarena_ds.h           # Arena allocation library ⭐
├── ds_api.h                # API template ⭐
├── ds_list.h               # Example: linked list ⭐
├── skeleton.bpf.c          # Kernel-side driver ⭐
├── skeleton.c              # Userspace driver ⭐
├── Makefile                # Build system ⭐
├── GUIDE.md                # Comprehensive guide ⭐
├── README.md               # This file
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
