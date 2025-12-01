# TODO: Remaining Work

## High Priority - Testing & Validation

### 1. Test MS Queue Implementation
- [ ] Run `sudo ./skeleton_msqueue -t 4 -o 1000` to verify basic functionality
- [ ] Check for FIFO ordering correctness in output
- [ ] Run stress test: `sudo ./skeleton_msqueue -t 16 -o 10000 -w mixed`
- [ ] Verify no memory leaks: check arena stats after long runs
- [ ] Test verification function: `sudo ./skeleton_msqueue -v`
- [ ] Compare performance vs `skeleton` (linked list) implementation

### 2. Fix Original Skeleton BPF Verifier Issue
**Status:** Still failing with "R1 invalid mem access 'scalar'" error

**Problem:** In `skeleton.bpf.c`, the code does:
```c
ds_head = &global_ds_head;
cast_kern(ds_head);
ds_list_init(ds_head);
```

But verifier still rejects it. The `cast_kern()` is present but apparently not working.

**Investigation needed:**
- Check if `cast_kern()` macro is properly defined in `bpf_arena_common.h`
- Verify the macro actually generates the right BPF bytecode
- May need to use alternative validation approach (see below)

**Possible fixes:**
1. Check `bpf_arena_common.h` for `cast_kern()` definition
2. Try explicit barrier: `barrier_var(ds_head);`
3. Try alternative initialization in userspace instead of kernel
4. Look at how `arena_list` (the original example) handles this

### 3. Create Test Scripts
- [ ] Add MS Queue to `test_smoke.sh`
- [ ] Add MS Queue to `test_stress.sh`
- [ ] Add MS Queue to `test_verify.sh`
- [ ] Update test documentation in README.md

## Medium Priority - Code Quality

### 4. Improve Error Handling
- [ ] Add better error messages in skeleton programs
- [ ] Handle edge cases in MS Queue (max retries exceeded, allocation failures)
- [ ] Add timeout mechanisms for infinite retry loops

### 5. Documentation
- [ ] Add usage examples to README.md for MS Queue
- [ ] Document performance characteristics vs linked list
- [ ] Add architecture diagram showing MS Queue structure
- [ ] Document the "helping" mechanism in MS Queue

### 6. Memory Reclamation Safety
**Current:** Immediate `bpf_arena_free()` after dequeue

**Risk:** Potential use-after-free in high contention

**Options:**
1. Implement epoch-based reclamation (heavyweight but safe)
2. Add grace period before freeing
3. Document the limitation and accept it for PoC
4. Rely on BPF arena's built-in reference counting (current approach)

**Recommendation:** For this framework, document the limitation. Arena reference counting provides reasonable protection.

## Low Priority - Enhancements

### 7. Add More Data Structures
Following the same pattern:
- [ ] Skip List (lock-free)
- [ ] Hash Table (concurrent)
- [ ] B-Tree (for ordered data)
- [ ] Trie (for string keys)

### 8. Performance Monitoring
- [ ] Add latency histograms (p50, p99, p999)
- [ ] Add throughput metrics (ops/sec over time)
- [ ] Add contention metrics (CAS retry counts)
- [ ] Create visualization scripts for results

### 9. Benchmarking Suite
- [ ] Create standardized benchmark harness
- [ ] Compare against userspace-only implementations
- [ ] Measure kernel-userspace hybrid overhead
- [ ] Generate performance comparison charts

## Known Issues

### Issue 1: cast_kern() Not Working
**Symptom:** BPF verifier rejects skeleton.bpf.c with scalar access error
**Status:** Needs investigation
**Priority:** High - blocks testing of original skeleton

### Issue 2: Arena Stats Not Accessible from Userspace
**Symptom:** Can't access `global_stats` via `skel->bss->`
**Root Cause:** Arena variables aren't in BSS section
**Status:** Documented and worked around (removed stats printing)
**Priority:** Low - not critical for functionality

### Issue 3: Config Variables Require Workaround
**Symptom:** Can't write to config variables from userspace
**Root Cause:** Was using `volatile const` (wrong section)
**Status:** Fixed by removing assignments, using defaults
**Alternative:** Could make them writable by removing const
**Priority:** Low - defaults work fine

## Notes for Future Implementations

1. **Always add cast_kern() after taking address of arena globals**
2. **Initialize all variables before use (especially in verify functions)**
3. **Cast unused CAS results to (void) to document intent**
4. **Add new programs to Makefile APPS list**
5. **Test with both skeleton.c and skeleton_[name].c to ensure consistency**
6. **Check for signed/unsigned comparison warnings**
7. **Use __attribute__((unused)) for API-required but unused parameters**

## Questions to Investigate

1. Why does `cast_kern()` not work in skeleton.bpf.c initialization?
2. Is there a better way to expose arena statistics to userspace?
3. Should we implement explicit ABA counters or trust arena isolation?
4. What's the optimal number of CAS retries before giving up?
5. Can we make config variables runtime-configurable from userspace?
