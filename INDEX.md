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
- **`README_FRAMEWORK.md`** - Framework overview
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
- **`ARCHITECTURE_DIAGRAMS.md`** - System diagrams
  - Component interaction
  - Memory layout
  - Data flow
  - Build pipeline

### Project Information
- **`PROJECT_SUMMARY.md`** - Build summary
  - What was built
  - Statistics
  - Design decisions
  - Completion status

---

## 💻 Source Code

### Core Library
- **`libarena_ds.h`** - Arena memory allocator
  - Per-CPU allocation
  - Statistics tracking
  - Atomic operations
  - ~300 lines, fully documented

### API Template
- **`ds_api.h`** - Standard interface
  - Operation definitions
  - Result codes
  - Statistics structures
  - Helper macros
  - ~400 lines with examples

### Reference Implementations
- **`ds_list.h`** - Doubly-linked list
  - Complete implementation
  - All API operations
  - Lock-free operations with atomics
  - ~352 lines

- **`ds_msqueue.h`** - Michael-Scott queue
  - Non-blocking FIFO queue
  - Lock-free enqueue/dequeue
  - Based on M&S 1996 paper
  - ~507 lines

### Test Framework
- **`skeleton.bpf.c`** - Kernel-side BPF program (list)
  - Arena map definition
  - LSM hook on inode_create
  - Operation dispatch
  - ~170 lines

- **`skeleton.c`** - Userspace test driver (list)
  - Single-threaded reader
  - Sleep then read pattern
  - Statistics collection
  - ~270 lines

- **`skeleton_msqueue.bpf.c`** - Kernel-side BPF program (MS queue)
  - Arena map definition
  - LSM hook on inode_create
  - MS queue operations
  - ~165 lines

- **`skeleton_msqueue.c`** - Userspace test driver (MS queue)
  - Single-threaded reader
  - Sleep then read pattern
  - Queue-specific operations
  - ~300 lines

### Build System
- **`Makefile.new`** - Annotated build system
  - All build stages documented
  - Multiple targets
  - ~350 lines

---

## 🧪 Testing

### Test Scripts (executable)
- **`test_smoke.sh`** - Quick validation (~30 seconds)
- **`test_stress.sh`** - Heavy load testing (~5 minutes)
- **`test_verify.sh`** - Correctness verification
- **`benchmark.sh`** - Performance measurement

### How to Run Tests
```bash
# Quick check
sudo ./test_smoke.sh

# Thorough testing
sudo ./test_stress.sh

# Verify correctness
sudo ./test_verify.sh

# Measure performance
sudo ./benchmark.sh
```

---

## 🔧 Reference Files (Original Examples)

These files are kept as reference examples:

- **`arena_list.bpf.c`** - Original BPF example
- **`arena_list.c`** - Original userspace example
- **`bpf_arena_list.h`** - Original list implementation
- **`bpf_arena_alloc.h`** - Original allocator
- **`bpf_arena_common.h`** - Common definitions (used)
- **`bpf_experimental.h`** - BPF features (used)

---

## 📖 Reading Order by Role

### "I want to use this framework"
1. `QUICKSTART.md` (5 min)
2. Build and run: `make -f Makefile.new && sudo ./skeleton -t 4 -o 1000`
3. `README_FRAMEWORK.md` (10 min)
4. Relevant sections of `GUIDE.md` as needed

### "I want to add a data structure"
1. `QUICKSTART.md` for basic setup
2. `GUIDE.md` → "Adding New Data Structures" section
3. Study `ds_list.h` as reference
4. Follow markers in `skeleton.bpf.c` and `skeleton.c`
5. Build and test

### "I want to understand the architecture"
1. `README_FRAMEWORK.md` → Architecture section
2. `ARCHITECTURE_DIAGRAMS.md` → All diagrams
3. `GUIDE.md` → Architecture section
4. Read source code with understanding of flow

### "I'm automating/building tools"
1. `GUIDE.md` → "Agent-Friendly Instructions"
2. `ds_api.h` → Study API patterns
3. `PROJECT_SUMMARY.md` → Design decisions
4. `ARCHITECTURE_DIAGRAMS.md` → Build pipeline

### "I'm debugging issues"
1. `GUIDE.md` → "Troubleshooting" section
2. Check test output and error messages
3. `ARCHITECTURE_DIAGRAMS.md` → Flow diagrams
4. Run with single thread: `sudo ./skeleton -t 1 -o 100`
5. Check `dmesg` for kernel messages

---

## 🎯 Quick Commands Reference

### Build
```bash
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
sudo ./test_smoke.sh          # Quick (~30s)
sudo ./test_stress.sh         # Thorough (~5min)
sudo ./test_verify.sh         # Correctness
sudo ./benchmark.sh           # Performance
make -f Makefile.new test     # Via Makefile
```

---

## 📁 Directory Structure

```
bpf_arena/
│
├── Documentation/
│   ├── QUICKSTART.md              ⭐ Start here for beginners
│   ├── README_FRAMEWORK.md        ⭐ Framework overview
│   ├── GUIDE.md                   ⭐ Complete reference
│   ├── ARCHITECTURE_DIAGRAMS.md   ⭐ Visual guides
│   ├── PROJECT_SUMMARY.md         ⭐ Build summary
│   ├── INDEX.md                   ⭐ This file
│   ├── README.md                  (Original example readme)
│   └── README.zh.md               (Chinese translation)
│
├── Core Library/
│   ├── libarena_ds.h              ⭐ Memory allocator
│   ├── ds_api.h                   ⭐ API template
│   ├── bpf_arena_common.h         Common definitions
│   └── bpf_experimental.h         BPF features
│
├── Implementations/
│   ├── ds_list.h                  ⭐ Reference: linked list
│   └── (add your ds_<name>.h here)
│
├── Test Framework/
│   ├── skeleton.bpf.c             ⭐ Kernel-side driver
│   ├── skeleton.c                 ⭐ Userspace driver
│   ├── Makefile.new               ⭐ Build system
│   └── Makefile                   (Original makefile)
│
├── Testing/
│   ├── test_smoke.sh              ⭐ Smoke tests
│   ├── test_stress.sh             ⭐ Stress tests
│   ├── test_verify.sh             ⭐ Verification tests
│   └── benchmark.sh               ⭐ Benchmarks
│
└── Reference Examples/
    ├── arena_list.bpf.c           Original BPF example
    ├── arena_list.c               Original userspace
    ├── bpf_arena_list.h           Original list impl
    └── bpf_arena_alloc.h          Original allocator
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
→ `ARCHITECTURE_DIAGRAMS.md` + `GUIDE.md` → "Architecture"

**...run tests?**
→ `README_FRAMEWORK.md` → "Running Tests"

**...fix build errors?**
→ `GUIDE.md` → "Troubleshooting" → "Compilation Issues"

**...fix runtime errors?**
→ `GUIDE.md` → "Troubleshooting" → "Runtime Issues"

**...interpret test output?**
→ `README_FRAMEWORK.md` → "Understanding Test Output"

**...improve performance?**
→ `GUIDE.md` → "Troubleshooting" → "Performance Issues"

**...automate/integrate?**
→ `GUIDE.md` → "Agent-Friendly Instructions"

### "What is..."

**...BPF arena?**
→ `GUIDE.md` → "Overview" or `README.md` (original tutorial)

**...the skeleton pattern?**
→ `README_FRAMEWORK.md` → "Architecture"

**...libarena_ds.h?**
→ Open `libarena_ds.h` - documented inline

**...ds_api.h?**
→ Open `ds_api.h` - has usage examples

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
Architecture → ARCHITECTURE_DIAGRAMS.md
```

### Debugging Chain
```
Error Occurred → Check error message
      ↓
Common Issue? → GUIDE.md → Troubleshooting
      ↓
Build Issue? → Check Makefile.new comments
      ↓
Runtime Issue? → Check dmesg: sudo dmesg | tail
      ↓
Logic Issue? → Review ARCHITECTURE_DIAGRAMS.md flows
```

---

## ✅ Checklist for Success

### Before Starting
- [ ] Read `QUICKSTART.md`
- [ ] Verified prerequisites (kernel 6.10+, clang 15+)
- [ ] Built successfully: `make -f Makefile.new`
- [ ] Ran basic test: `sudo ./skeleton -t 4 -o 1000`

### Adding Data Structure
- [ ] Created `ds_<name>.h` with all API operations
- [ ] Modified `skeleton.bpf.c` at marked insertion points
- [ ] Modified `skeleton.c` to use new data structure
- [ ] Built without errors
- [ ] Passed smoke tests: `sudo ./test_smoke.sh`
- [ ] Passed verification: `sudo ./skeleton -t 4 -o 1000 -v`

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
3. Experiment with different options (-t, -o, -w)
4. Understand the output

### Level 2: Developer (4 hours)
1. Read `GUIDE.md` thoroughly
2. Study `ds_list.h` implementation
3. Add a simple data structure (e.g., stack)
4. Run all tests

### Level 3: Expert (1 day)
1. Understand arena allocator (`libarena_ds.h`)
2. Study BPF program (`skeleton.bpf.c`)
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
1. make -f Makefile.new clean
2. make -f Makefile.new
3. sudo ./test_smoke.sh
4. sudo ./skeleton -t 4 -o 1000 -v
5. sudo ./benchmark.sh
```

### Development Workflow
```bash
1. Edit ds_<name>.h
2. Update skeleton.bpf.c (marked points)
3. Update skeleton.c (types and calls)
4. make -f Makefile.new
5. sudo ./skeleton -t 1 -o 100  # Quick test
6. sudo ./skeleton -t 4 -o 1000 -v  # Full test
7. sudo ./test_smoke.sh  # Validation
```

### Debugging Workflow
```bash
1. Reproduce issue with minimal test
2. Check GUIDE.md troubleshooting
3. Run with V=1: make -f Makefile.new V=1
4. Check kernel logs: sudo dmesg | tail -50
5. Run single-threaded: sudo ./skeleton -t 1 -o 10
6. Add debug prints (bpf_printk in BPF, printf in userspace)
```

---

## 📝 File Size Reference

| File | Type | Lines | Purpose |
|------|------|-------|---------|
| `libarena_ds.h` | Code | ~300 | Memory allocator |
| `ds_api.h` | Code | ~400 | API template |
| `ds_list.h` | Code | ~450 | List implementation |
| `skeleton.bpf.c` | Code | ~300 | Kernel driver |
| `skeleton.c` | Code | ~700 | Userspace driver |
| `Makefile.new` | Build | ~350 | Build system |
| `GUIDE.md` | Doc | ~1000 | Complete guide |
| `QUICKSTART.md` | Doc | ~400 | Quick start |
| `README_FRAMEWORK.md` | Doc | ~350 | Overview |
| Test scripts | Shell | ~400 | Testing |

**Total**: ~4,650 lines of code and documentation

---

## 🎉 You're Ready!

Pick your starting point based on your goal:

- **Just want to use it?** → Start with `QUICKSTART.md`
- **Want to understand it?** → Start with `ARCHITECTURE_DIAGRAMS.md`
- **Want to extend it?** → Start with `GUIDE.md` section on adding data structures
- **Want to automate it?** → Start with `GUIDE.md` agent instructions

**Happy testing!** 🚀
