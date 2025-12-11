# BPF Arena Data Structures Framework - Index

**Quick Navigation Guide**

---

## 📚 Documentation (Start Here!)

### For Absolute Beginners
- **`QUICKSTART.md`** - Get started in 5 minutes
  - Prerequisites check
  - Build and run
  - Understanding output
  - Common issues
  - Success checklist

### For Users
- **`README.md`** - Framework overview
  - What it does
  - Quick start
  - Usage examples
  - Command-line options
  - Key features

### For Developers
- **`GUIDE.md`** - Comprehensive reference (READ THIS!)
  - Architecture details
  - Step-by-step guide to add data structures
  - Running tests
  - Nomenclature and conventions
  - Troubleshooting
  - Agent-friendly instructions

### Visual References
- **`.agent/ARCHITECTURE_DIAGRAMS.md`** - System diagrams
  - Component interaction
  - Memory layout
  - Data flow
  - Build pipeline

---

## 💻 Source Code

### Core Library
- **`include/libarena_ds.h`** - Arena memory allocator
  - Per-CPU allocation
  - Statistics tracking
  - Atomic operations
  - ~300 lines, fully documented

### API Template
- **`include/ds_api.h`** - Standard interface
  - Operation definitions
  - Result codes
  - Statistics structures
  - Helper macros
  - ~400 lines with examples

### Reference Implementations
- **`include/ds_list.h`** - Doubly-linked list
  - Complete implementation
  - All API operations
  - Lock-free operations with atomics
  - ~352 lines

- **`include/ds_msqueue.h`** - Michael-Scott queue
  - Non-blocking FIFO queue
  - Lock-free enqueue/dequeue
  - Based on M&S 1996 paper
  - ~507 lines

### Test Framework
- **`src/skeleton.bpf.c`** - Kernel-side BPF program (list)
  - Arena map definition
  - LSM hook on inode_create
  - Operation dispatch
  - ~170 lines

- **`src/skeleton.c`** - Userspace test driver (list)
  - Single-threaded reader
  - Sleep then read pattern
  - Statistics collection
  - ~270 lines

- **`src/skeleton_msqueue.bpf.c`** - Kernel-side BPF program (MS queue)
  - Arena map definition
  - LSM hook on inode_create
  - MS queue operations
  - ~165 lines

- **`src/skeleton_msqueue.c`** - Userspace test driver (MS queue)
  - Single-threaded reader
  - Sleep then read pattern
  - Queue-specific operations
  - ~300 lines

### Build System
- **`Makefile`** - Build system
  - All build stages documented
  - Multiple targets
  - ~350 lines

---

## 🧪 Testing

### Test Scripts (executable)
- **`scripts/test_smoke.sh`** - Quick validation (~30 seconds)
- **`scripts/test_stress.sh`** - Heavy load testing (~5 minutes)
- **`scripts/test_verify.sh`** - Correctness verification
- **`scripts/benchmark.sh`** - Performance measurement

### How to Run Tests
```bash
# Quick check
sudo ./scripts/test_smoke.sh

# Thorough testing
sudo ./scripts/test_stress.sh

# Verify correctness
sudo ./scripts/test_verify.sh

# Measure performance
sudo ./scripts/benchmark.sh
```

---

## 🔧 Common Definitions

These files provide BPF compatibility:

- **`include/bpf_arena_common.h`** - Common definitions for BPF/userspace
- **`include/bpf_experimental.h`** - BPF experimental features

---

## 📖 Reading Order by Role

### "I want to use this framework"
1. `QUICKSTART.md` (5 min)
2. Build and run: `make && sudo ./skeleton -d 5`
3. `README.md` (10 min)
4. Relevant sections of `GUIDE.md` as needed

### "I want to add a data structure"
1. `QUICKSTART.md` for basic setup
2. `GUIDE.md` → "Adding New Data Structures" section
3. Study `include/ds_list.h` as reference
4. Follow markers in `src/skeleton.bpf.c` and `src/skeleton.c`
5. Build and test

### "I want to understand the architecture"
1. `README.md` → Architecture section
2. `.agent/ARCHITECTURE_DIAGRAMS.md` → All diagrams
3. `GUIDE.md` → Architecture section
4. Read source code with understanding of flow

### "I'm automating/building tools"
1. `GUIDE.md` → "Agent-Friendly Instructions"
2. `include/ds_api.h` → Study API patterns
3. `.agent/ARCHITECTURE_DIAGRAMS.md` → Build pipeline

### "I'm debugging issues"
1. `GUIDE.md` → "Troubleshooting" section
2. Check test output and error messages
3. `.agent/ARCHITECTURE_DIAGRAMS.md` → Flow diagrams
4. Run with short duration: `sudo ./skeleton -d 1`
5. Check `dmesg` for kernel messages

---

## 🎯 Quick Commands Reference

### Build
```bash
# First time only: initialize submodules
git submodule update --init --recursive

make              # Build all
make skeleton     # Build skeleton only
make clean        # Clean build
make V=1          # Verbose build
make help         # Show help
```

### Run
```bash
# Basic usage (sleep 5 seconds while kernel populates)
sudo ./skeleton -d 5

# Longer collection period
sudo ./skeleton -d 30

# With verification
sudo ./skeleton -d 5 -v

# With statistics
sudo ./skeleton -d 10 -s
```

### Test
```bash
sudo ./scripts/test_smoke.sh          # Quick (~30s)
sudo ./scripts/test_stress.sh         # Thorough (~5min)
sudo ./scripts/test_verify.sh         # Correctness
sudo ./scripts/benchmark.sh           # Performance
make test                             # Via Makefile
```

---

## 📁 Directory Structure

```
bpf-arena-data-structures/
│
├── Documentation/
│   ├── QUICKSTART.md              ⭐ Start here for beginners
│   ├── README.md                  ⭐ Framework overview
│   ├── GUIDE.md                   ⭐ Complete reference
│   ├── INDEX.md                   ⭐ This file
│   └── .agent/
│       └── ARCHITECTURE_DIAGRAMS.md   Visual guides
│
├── include/                       ⭐ Header files
│   ├── libarena_ds.h              ⭐ Memory allocator
│   ├── ds_api.h                   ⭐ API template
│   ├── ds_list.h                  ⭐ Reference: linked list
│   ├── ds_msqueue.h               ⭐ Michael-Scott queue
│   ├── bpf_arena_common.h         Common definitions
│   └── bpf_experimental.h         BPF features
│
├── src/                           ⭐ Source files
│   ├── skeleton.bpf.c             ⭐ Kernel-side driver (list)
│   ├── skeleton.c                 ⭐ Userspace driver (list)
│   ├── skeleton_msqueue.bpf.c     ⭐ Kernel-side driver (queue)
│   └── skeleton_msqueue.c         ⭐ Userspace driver (queue)
│
├── scripts/                       ⭐ Test scripts
│   ├── test_smoke.sh              ⭐ Smoke tests
│   ├── test_stress.sh             ⭐ Stress tests
│   ├── test_verify.sh             ⭐ Verification tests
│   └── benchmark.sh               ⭐ Benchmarks
│
├── Makefile                       ⭐ Build system
│
└── third_party/                   Dependencies
    ├── libbpf/
    ├── bpftool/
    └── vmlinux/
```

⭐ = Core framework files you'll use

---

## 🔍 Finding Information Fast

### "How do I..."

**...get started?**
→ `QUICKSTART.md`

**...add a new data structure?**
→ `GUIDE.md` → "Adding New Data Structures"

**...understand the architecture?**
→ `.agent/ARCHITECTURE_DIAGRAMS.md` + `GUIDE.md` → "Architecture"

**...run tests?**
→ `README.md` → "Running Tests"

**...fix build errors?**
→ `GUIDE.md` → "Troubleshooting" → "Compilation Issues"

**...fix runtime errors?**
→ `GUIDE.md` → "Troubleshooting" → "Runtime Issues"

**...interpret test output?**
→ `README.md` → "Understanding Test Output"

**...improve performance?**
→ `GUIDE.md` → "Troubleshooting" → "Performance Issues"

**...automate/integrate?**
→ `GUIDE.md` → "Agent-Friendly Instructions"

### "What is..."

**...BPF arena?**
→ `GUIDE.md` → "Overview" or `README.md` (original tutorial)

**...the skeleton pattern?**
→ `README.md` → "Architecture"

**...libarena_ds.h?**
→ Open `include/libarena_ds.h` - documented inline

**...ds_api.h?**
→ Open `include/ds_api.h` - has usage examples

**...the expected output?**
→ `QUICKSTART.md` → "Understanding the Output"

---

## 📞 Support Resources

### Documentation Chain
```
Quick Question → QUICKSTART.md
      ↓
Detailed Info → GUIDE.md (relevant section)
      ↓
Still Stuck → Read source code comments
      ↓
Architecture → .agent/ARCHITECTURE_DIAGRAMS.md
```

### Debugging Chain
```
Error Occurred → Check error message
      ↓
Common Issue? → GUIDE.md → Troubleshooting
      ↓
Build Issue? → Check Makefile comments
      ↓
Runtime Issue? → Check dmesg: sudo dmesg | tail
      ↓
Logic Issue? → Review .agent/ARCHITECTURE_DIAGRAMS.md flows
```

---

## ✅ Checklist for Success

### Before Starting
- [ ] Read `QUICKSTART.md`
- [ ] Verified prerequisites (kernel 6.10+, clang 15+)
- [ ] Built successfully: `make`
- [ ] Ran basic test: `sudo ./skeleton -d 5`

### Adding Data Structure
- [ ] Created `include/ds_<name>.h` with all API operations
- [ ] Modified `src/skeleton.bpf.c` at marked insertion points
- [ ] Modified `src/skeleton.c` to use new data structure
- [ ] Built without errors
- [ ] Passed smoke tests: `sudo ./scripts/test_smoke.sh`
- [ ] Passed verification: `sudo ./skeleton -d 5 -v`

### Before Production
- [ ] Passed all smoke tests
- [ ] Passed all stress tests
- [ ] Passed all verification tests
- [ ] Benchmarked performance
- [ ] Documented any custom operations
- [ ] Tested edge cases

---

## 🎓 Learning Path

### Level 1: User (1 hour)
1. Read `QUICKSTART.md`
2. Build and run basic test
3. Experiment with different options (-d duration, -v verify, -s stats)
4. Understand the output

### Level 2: Developer (4 hours)
1. Read `GUIDE.md` thoroughly
2. Study `include/ds_list.h` implementation
3. Add a simple data structure (e.g., stack)
4. Run all tests

### Level 3: Expert (1 day)
1. Understand arena allocator (`include/libarena_ds.h`)
2. Study BPF program (`src/skeleton.bpf.c`)
3. Add complex data structure (e.g., AVL tree)
4. Optimize for performance
5. Write custom tests

### Level 4: Contributor (ongoing)
1. Add new features
2. Improve documentation
3. Optimize existing code
4. Share with community

---

## 🚀 Common Workflows

### Testing Workflow
```bash
1. make clean
2. make
3. sudo ./scripts/test_smoke.sh
4. sudo ./skeleton -d 5 -v
5. sudo ./scripts/benchmark.sh
```

### Development Workflow
```bash
1. Edit include/ds_<name>.h
2. Update src/skeleton.bpf.c (marked points)
3. Update src/skeleton.c (types and calls)
4. make
5. sudo ./skeleton -d 1  # Quick test
6. sudo ./skeleton -d 5 -v  # Full test
7. sudo ./scripts/test_smoke.sh  # Validation
```

### Debugging Workflow
```bash
1. Reproduce issue with minimal test
2. Check GUIDE.md troubleshooting
3. Run with V=1: make V=1
4. Check kernel logs: sudo dmesg | tail -50
5. Run short duration: sudo ./skeleton -d 1
6. Add debug prints (bpf_printk in BPF, printf in userspace)
```

---

## 📝 File Size Reference

| File | Type | Lines | Purpose |
|------|------|-------|---------|
| `include/libarena_ds.h` | Code | ~300 | Memory allocator |
| `include/ds_api.h` | Code | ~400 | API template |
| `include/ds_list.h` | Code | ~450 | List implementation |
| `include/ds_msqueue.h` | Code | ~507 | MS Queue implementation |
| `src/skeleton.bpf.c` | Code | ~170 | Kernel driver (list) |
| `src/skeleton.c` | Code | ~270 | Userspace driver (list) |
| `src/skeleton_msqueue.bpf.c` | Code | ~165 | Kernel driver (queue) |
| `src/skeleton_msqueue.c` | Code | ~300 | Userspace driver (queue) |
| `Makefile` | Build | ~350 | Build system |
| `GUIDE.md` | Doc | ~800 | Complete guide |
| `QUICKSTART.md` | Doc | ~320 | Quick start |
| `README.md` | Doc | ~350 | Overview |
| Test scripts | Shell | ~400 | Testing |

**Total**: ~4,800 lines of code and documentation

---

## 🎉 You're Ready!

Pick your starting point based on your goal:

- **Just want to use it?** → Start with `QUICKSTART.md`
- **Want to understand it?** → Start with `.agent/ARCHITECTURE_DIAGRAMS.md`
- **Want to extend it?** → Start with `GUIDE.md` section on adding data structures
- **Want to automate it?** → Start with `GUIDE.md` agent instructions

**Happy testing!** 🚀
