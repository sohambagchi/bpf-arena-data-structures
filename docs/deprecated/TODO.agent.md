# TODO: Remaining Work

## Current Status

✅ **Core framework is WORKING**
- Skeleton list implementation compiles and loads successfully
- LSM-based kernel insertion works correctly
- Userspace can read from arena directly
- Simplified bump allocator functioning
- No statistics tracking (intentionally removed for simplicity)

## High Priority - Testing & Validation

### 1. Test Current Skeleton Implementation
- [ ] Run basic test: `sudo ./skeleton -d 5`
- [ ] Test with verification: `sudo ./skeleton -d 5 -v`
- [ ] Run longer duration: `sudo ./skeleton -d 30`
- [ ] Verify data structure integrity across multiple runs
- [ ] Check for memory leaks during extended runs

### 2. MS Queue Implementation (INCOMPLETE - PAUSED)
**Status:** Not yet functional, paused for now

**What's there:**
- `ds_msqueue.h` - Header with MS Queue algorithm implementation
- `skeleton_msqueue.bpf.c` and `skeleton_msqueue.c` - Skeleton files

**What's needed:**
- Testing and debugging
- Verification of lock-free properties
- Performance comparison with list implementation

### 3. Test Scripts Status
✅ Test scripts exist:
- `test_smoke.sh` - Basic validation tests
- `test_stress.sh` - Heavy load tests  
- `test_verify.sh` - Correctness checks
- `benchmark.sh` - Performance measurements

- [ ] Verify all test scripts work with current simplified implementation
- [ ] Update test scripts to match new LSM-based execution model (no more -t, -o flags)
- [ ] Add MS Queue to test scripts (when ready)

## Medium Priority - Code Quality

### 4. Documentation
✅ Core documentation complete:
- `GUIDE.md` - Comprehensive framework guide
- `README.md` - Quick reference
- `QUICKSTART.md` - Beginner guide
- `docs/ARCHITECTURE_DIAGRAMS.md` - Visual reference

- [ ] Update all docs to reflect simplified LSM-based model (no statistics, no multi-threaded userspace)
- [ ] Remove references to non-existent features (statistics tracking removed)
- [ ] Add examples of the actual working skeleton program usage
- [ ] Document MS Queue when ready

### 5. Memory Reclamation Safety
**Current:** Simple bump allocator - no actual freeing

**Status:** Working approach for the framework's needs
- Memory is reused when program restarts
- No complex reclamation needed for current use case
- `bpf_arena_free()` is a no-op placeholder

**Future:** If implementing true free(), consider:
1. Epoch-based reclamation for lock-free structures
2. Reference counting for shared nodes
3. Grace periods before freeing

## Low Priority - Enhancements

### 6. Add More Data Structures
Following the same pattern:
- [ ] Complete MS Queue (lock-free FIFO)
- [ ] Skip List (lock-free ordered)
- [ ] Hash Table (concurrent)
- [ ] B-Tree (for ordered data)

### 7. Expand Testing Capabilities
- [ ] Add configurable workload generator for userspace
- [ ] Support for multi-threaded userspace operations (current is single-threaded reader)
- [ ] More comprehensive verification functions
- [ ] Memory usage tracking and leak detection

## Known Limitations (Not Bugs)

### 1. No Statistics Tracking
**Status:** Intentionally removed for simplicity
**Reason:** Simplified the codebase by ~250 lines
**Impact:** Can't measure per-operation timing or detailed metrics
**Workaround:** Manual analysis of data structure state

### 2. Bump Allocator Only
**Status:** Simple allocation strategy
**Reason:** Avoids complex memory management
**Impact:** Memory not truly freed until program restart
**Workaround:** Acceptable for testing framework

### 3. Single-threaded Userspace Reader
**Status:** By design in simplified model
**Reason:** Focuses on kernel-side insertion testing
**Impact:** Can't test heavy concurrent userspace operations
**Workaround:** Can be extended if needed

## Notes for Future Implementations

1. **With clang-20, cast_kern()/cast_user() are NOPs** - LLVM handles address space casts automatically
2. **Initialize all variables before use** (especially in verify functions)
3. **Cast unused CAS results to (void)** to document intent
4. **Add new programs to Makefile APPS list**
5. **Check for signed/unsigned comparison warnings**
6. **Use __attribute__((unused))** for API-required but unused parameters
7. **LSM hooks must return 0** to allow the operation to proceed
8. **Bump allocator is simple but effective** for this use case

## Questions for Future Exploration

1. Can we efficiently expose data structure state to userspace without complex IPC?
2. Should we add support for multi-threaded userspace operations?
3. What's the best way to trigger kernel operations reliably for testing?
4. How to implement true memory freeing without complex reclamation schemes?
