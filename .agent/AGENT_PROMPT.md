# Prompt

```
Implement [DATA_STRUCTURE_NAME] for this BPF arena testing framework.

1. Create `ds_[name].h` following the ds_api.h contract (study ds_list.h as reference).

2. Create NEW skeleton files `skeleton_[name].bpf.c` and `skeleton_[name].c` based on the existing skeletons - copy them and replace all `ds_list_*` calls with `ds_[name]_*` calls (look for /* DS_API_* */ comments as markers). Update #include to use your new header.

DO NOT modify the original skeleton.bpf.c or skeleton.c files.

Key requirements:
- All operations must update stats (count, failures, total_time_ns)
- Use __arena for arena pointers, cast_kern()/cast_user() appropriately  
- Implement verify() to check structural integrity
- Use can_loop in all loops
- Memory via bpf_arena_alloc()/bpf_arena_free()

See GUIDE.md for details. Ask questions if the specification is unclear.
```

## Critical Lessons Learned

### Compilation Issues to Avoid

1. **Arena Variables Not in BSS Section**
   - Variables declared with `__arena` (e.g., `struct foo __arena global_var;`) live in arena memory, NOT in the BPF program's BSS section
   - **Cannot access via skeleton:** `skel->bss->global_stats` will fail if `global_stats` is `__arena`
   - **Fix:** Remove arena variable access from userspace, or declare without `__arena` if userspace needs access

2. **Config Variables Must Be Writable from Userspace**
   - Do NOT use `volatile const` for variables userspace needs to write (they go to `.rodata` which is read-only)
   - **Correct:** `int config_value = 100;` (goes to `.bss`, writable)
   - **Wrong:** `volatile const int config_value = 100;` (goes to `.rodata`, read-only)
   - **Alternative:** Skip writing config from userspace, use BPF defaults

3. **BPF Verifier Requires cast_kern() for Arena Pointers**
   - When taking address of arena global: `ds_head = &global_ds_head;`
   - **MUST call `cast_kern(ds_head);` immediately after** to validate pointer for verifier
   - Without it, verifier sees a scalar and rejects memory access with "R1 invalid mem access 'scalar'"

4. **Dual-Context Headers Need Conditional Compilation**
   - Headers included by both BPF and userspace need `#ifdef __BPF__` guards
   - **can_loop macro:** Defined by `bpf_experimental.h` in BPF context, but undefined in userspace
   - **Fix in libarena_ds.h:**
     ```c
     #ifndef can_loop
     #define can_loop 1
     #endif
     ```

5. **Initialize All Variables Used in Conditional Paths**
   - Compiler warning `-Wmaybe-uninitialized` often indicates real bugs
   - Example: `expected_pprev` in `ds_list_verify()` must be initialized to `&head->first`

6. **Mark Intentionally Unused CAS Results**
   - In lock-free algorithms, some CAS operations are "helping" moves where failure is acceptable
   - Cast to `(void)` to silence warnings: `(void)arena_atomic_cmpxchg(&head->tail, tail, node);`
   - Documents that ignoring the result is intentional, not a bug

7. **Unused Parameters in API Conformance**
   - MS Queue `delete()` doesn't use `key` (FIFO, not keyed), but `ds_api.h` requires it
   - **Fix:** Add `__attribute__((unused))` to parameter declaration

8. **Add New Apps to Makefile**
   - After creating `skeleton_[name].bpf.c` and `skeleton_[name].c`
   - **Must add to:** `APPS = skeleton skeleton_[name]` in Makefile
   - Otherwise `make` won't build the new programs

### Toolchain Requirements

- **clang-20 or newer** required for full BPF arena support with atomic operations
- clang-11 crashes on atomic operations on arena memory (LLVM backend bug)
- Update in Makefile: `CLANG ?= clang-20`

## Files to Include

1. `ds_api.h` - API contract
2. `libarena_ds.h` - Memory allocator
3. `ds_list.h` - Reference implementation
4. `skeleton.bpf.c` - Kernel skeleton
5. `skeleton.c` - Userspace skeleton
6. `GUIDE.md` - Framework guide
7. [Your data structure specification document]