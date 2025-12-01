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

## Files to Include

1. `ds_api.h` - API contract
2. `libarena_ds.h` - Memory allocator
3. `ds_list.h` - Reference implementation
4. `skeleton.bpf.c` - Kernel skeleton
5. `skeleton.c` - Userspace skeleton
6. `GUIDE.md` - Framework guide
7. [Your data structure specification document]