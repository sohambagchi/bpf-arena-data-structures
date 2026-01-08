# BPF Arena Concurrent Data Structures Testing Framework

A comprehensive framework for testing concurrent data structures using **BPF arena** - enabling operations from both kernel-space (BPF programs) and user-space (direct arena access) with zero-copy shared memory.

## 🎯 Quick Start

```bash
# Initialize third-party dependencies (first time only)
git submodule update --init --recursive

# Build everything
make

# Run basic test (kernel LSM hook populates, userspace polls continuously)
sudo ./skeleton

# Run with verification  
sudo ./skeleton -v

# See comprehensive guide
cat docs/GUIDE.md
```

## 📋 What's Included

### Core Library Components

- **`libarena_ds.h`** - Simple bump allocator for arena memory
- **`ds_api.h`** - Standard API template for data structure implementations
- **`bpf_arena_common.h`** - Common definitions for BPF/userspace compatibility
- **`bpf_experimental.h`** - BPF experimental features

### Test Framework

- **`skeleton.bpf.c`** - Kernel-side BPF program with LSM hooks (inserts on file creation)
- **`skeleton.c`** - Userspace reader program (single-threaded)
- **`skeleton_msqueue.bpf.c`** - Kernel-side BPF program for MS Queue
- **`skeleton_msqueue.c`** - Userspace reader for MS Queue
- **`skeleton_bst.bpf.c`** - Kernel-side BPF program for Binary Search Tree
- **`skeleton_bst.c`** - Userspace reader for Binary Search Tree
- **`skeleton_bintree.bpf.c`** - Kernel-side BPF program for Ellen's Binary Tree
- **`skeleton_bintree.c`** - Userspace reader for Ellen's Binary Tree
- **`skeleton_mpsc.bpf.c`** - Kernel-side BPF program for MPSC Queue
- **`skeleton_mpsc.c`** - Userspace reader for MPSC Queue
- **`skeleton_vyukhov.bpf.c`** - Kernel-side BPF program for Vyukhov MPSC Queue
- **`skeleton_vyukhov.c`** - Userspace reader for Vyukhov MPSC Queue
- **`skeleton_folly_spsc.bpf.c`** - Kernel-side BPF program for Folly SPSC Queue
- **`skeleton_folly_spsc.c`** - Userspace reader for Folly SPSC Queue
- **`ds_list.h`** - Reference implementation (doubly-linked list)
- **`ds_msqueue.h`** - Michael-Scott lock-free queue implementation
- **`ds_bst.h`** - Binary Search Tree implementation
- **`ds_bintree.h`** - Ellen's Binary Tree implementation
- **`ds_mpsc.h`** - MPSC Queue implementation
- **`ds_vyukhov.h`** - Vyukhov MPSC Queue implementation
- **`ds_folly_spsc.h`** - Folly SPSC Queue implementation

### Testing Infrastructure

**Note:** Test scripts reference workload parameters that the current simplified implementation doesn't use. They serve as templates for future multi-threaded implementations.

- **`test_smoke.sh`** - Quick smoke tests (template)
- **`test_stress.sh`** - Stress tests (template)
- **`test_verify.sh`** - Correctness verification (template)
- **`benchmark.sh`** - Performance benchmarking (template)

### Documentation

- **`README.md`** - This file
- **`docs/GUIDE.md`** - Comprehensive guide (architecture, adding data structures, conventions)
- **`QUICKSTART.md`** - Quick start guide
- **`INDEX.md`** - Navigation index
- **`docs/ARCHITECTURE_DIAGRAMS.md`** - Visual diagrams

### Build System

- **`Makefile`** - Build system

## 🏗️ Architecture

```
┌─────────────────────────────────────────┐
│         USER SPACE                      │
│  Continuous Polling Reader              │
│  Direct arena access (Zero Copy)        │
│  Dequeues and prints elements           │
├─────────────────────────────────────────┤
│         BPF ARENA                       │
│  Shared memory region (up to 4GB)       │
│  Accessible from both contexts          │
│  Sync via arena_atomic_* API            │
├─────────────────────────────────────────┤
│         KERNEL SPACE                    │
│  BPF programs via LSM hooks             │
│  Inserts triggered by file creation     │
│  (lsm.s/inode_create hook)              │
└─────────────────────────────────────────┘
```

## 🚀 Usage Examples

### Basic Testing

```bash
# Run and poll for data (Ctrl+C to stop)
sudo ./skeleton

# Run with data structure verification on exit
sudo ./skeleton -v

# Run with statistics output (enabled by default)
sudo ./skeleton -s
```

### Testing MS Queue

```bash
# Test Michael-Scott lock-free queue
sudo ./skeleton_msqueue

# With verification
sudo ./skeleton_msqueue -v
```

### Testing Other Data Structures

```bash
# Binary Search Tree
sudo ./skeleton_bst

# Ellen's Binary Tree
sudo ./skeleton_bintree

# MPSC Queue
sudo ./skeleton_mpsc

# Vyukhov MPSC Queue
sudo ./skeleton_vyukhov
```

### Automated Testing

**Note:** These test scripts are templates for future multi-threaded implementations.

```bash
# Test scripts (templates - may need adjustment for current design)
# sudo ./test_smoke.sh
# sudo ./test_stress.sh
# sudo ./test_verify.sh
# sudo ./benchmark.sh
```

### Command-Line Options

```
./skeleton [OPTIONS]
./skeleton_msqueue [OPTIONS]

DESIGN:
  Kernel:    LSM hook on inode_create inserts items (triggers on file creation)
  Userspace: Continuously polls and dequeues elements as they arrive

OPTIONS:
  -v      Verify data structure integrity on exit
  -s      Print statistics on exit (default: enabled)
  -h      Show this help

NOTE: Kernel inserts trigger automatically when files are created on the system.
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

### 1. Create Data Structure Header (`include/ds_<name>.h`)

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

### 2. Modify `src/skeleton.bpf.c`

Look for `/* DS_API_INSERT */` markers and add:
- Include your header
- Declare your data structure head
- Update operation dispatch in `handle_operation()`

### 3. Modify `src/skeleton.c`

- Include your header
- Update worker context type
- Update workload functions to use your API

### 4. Build and Test

```bash
make clean && make
sudo ./skeleton -v
```

**See `docs/GUIDE.md` for detailed step-by-step instructions.**

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
3. Userspace programs continuously poll and dequeue from the data
   structure (the standard pattern in this repo's `skeleton_*` binaries).
   See per-binary behavior in the `src/` files (e.g., `src/skeleton.c`,
   `src/skeleton_msqueue.c`).
4. After collection, userspace prints statistics and optionally verifies
   integrity.

## 📁 File Structure

```
bpf-arena-data-structures/
├── Core Framework
│   ├── libarena_ds.h           # Arena allocation library ⭐
│   ├── ds_api.h                # API template ⭐
│   ├── bpf_arena_common.h      # Common definitions ⭐
│   └── bpf_experimental.h      # BPF experimental features
│
├── Data Structure Implementations
│   ├── ds_list.h               # Doubly-linked list ⭐
│   ├── ds_msqueue.h            # Michael-Scott queue ⭐
│   ├── ds_bst.h                # Binary Search Tree
│   ├── ds_bintree.h            # Ellen's Binary Tree
│   ├── ds_mpsc.h               # MPSC Queue
│   └── ds_vyukhov.h            # Vyukhov MPSC Queue
│
├── Test Programs
│   ├── skeleton.bpf.c          # Kernel BPF program (list) ⭐
│   ├── skeleton.c              # Userspace reader (list) ⭐
│   ├── skeleton_msqueue.bpf.c  # Kernel BPF program (queue) ⭐
│   ├── skeleton_msqueue.c      # Userspace reader (queue) ⭐
│   ├── skeleton_bst.bpf.c      # Kernel BPF program (BST)
│   ├── skeleton_bst.c          # Userspace reader (BST)
│   ├── skeleton_bintree.bpf.c  # Kernel BPF program (Ellen's Tree)
│   ├── skeleton_bintree.c      # Userspace reader (Ellen's Tree)
│   ├── skeleton_mpsc.bpf.c     # Kernel BPF program (MPSC)
│   ├── skeleton_mpsc.c         # Userspace reader (MPSC)
│   ├── skeleton_vyukhov.bpf.c  # Kernel BPF program (Vyukhov)
│   └── skeleton_vyukhov.c      # Userspace reader (Vyukhov)
│
├── Build System
│   └── Makefile                # Build system ⭐
│
├── Documentation
│   ├── README.md                     # This file
│   ├── docs/GUIDE.md                 # Comprehensive guide ⭐
│   ├── QUICKSTART.md                 # Quick start guide
│   ├── INDEX.md                      # Navigation index
│   └── docs/ARCHITECTURE_DIAGRAMS.md # Visual diagrams
│
├── Test Scripts
│   ├── scripts/test_smoke.sh           # Smoke tests
│   ├── scripts/test_stress.sh          # Stress tests
│   ├── scripts/test_verify.sh          # Verification tests
│   └── scripts/benchmark.sh            # Performance benchmarks
│
└── Third Party Dependencies
    └── third_party/
        ├── vmlinux.h           # Kernel type definitions
        ├── libbpf/             # BPF library
        ├── bpftool/            # BPF tool
        └── vmlinux/            # Architecture-specific vmlinux headers
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

- **Comprehensive Guide**: See `docs/GUIDE.md` for detailed documentation
- **Linux BPF Arena**: `Documentation/bpf/arena.rst` in kernel source
- **libbpf**: https://libbpf.readthedocs.io/

## 🤝 For Agents and Automation

This framework is designed to be agent-friendly:

- **Standardized API**: All data structures follow `ds_api.h` template
- **Clear insertion points**: Look for `/* DS_API_INSERT */` markers
- **JSON schema**: See `docs/GUIDE.md` for structured metadata format
- **Testing checklist**: See `docs/GUIDE.md` for automated testing guidance
- **Common patterns**: Reference implementations in `ds_list.h`

## ⚡ Quick Reference

```bash
# First time setup
git submodule update --init --recursive

# Build
make

# Test
sudo ./skeleton -v

# Automated testing
sudo ./scripts/test_smoke.sh      # Quick (~30s)
sudo ./scripts/test_stress.sh     # Thorough (~5min)
sudo ./scripts/test_verify.sh     # Correctness
sudo ./scripts/benchmark.sh       # Performance

# Clean
make clean
```

## 📝 License

SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause)

Based on BPF arena examples from the Linux kernel, Copyright (c) 2024 Meta Platforms, Inc. and affiliates.

---

**Ready to test concurrent data structures? Start with `docs/GUIDE.md` for comprehensive documentation!**
