# Assembly Memory Operations and Control Flow Analysis Prompt

## Task Description
This prompt provides instructions for analyzing x86-64 assembly code to identify memory operations and trace control flow paths through a function.

## Instructions

When analyzing assembly code (typically from a `.S` file in the `build/` directory):

### 1. Identify Memory Operations
For each memory operation in the assembly code:
- **Label it sequentially**: m00, m01, m02, m03, etc.
- **Identify the type**: LOAD, STORE, or CAS (compare-and-swap)
- **Locate the instruction**: Provide the address and instruction
- **Reference the C code**: Include the corresponding C code hint from comments
- **Note memory ordering**: Identify acquire/release semantics if present

**Memory operation indicators:**
- `mov reg, QWORD/DWORD/WORD/BYTE PTR [memory]` → LOAD
- `mov QWORD/DWORD/WORD/BYTE PTR [memory], reg` → STORE
- `lock cmpxchg`, `lock xchg` → CAS/atomic operation
- Function calls to `smp_load_acquire`, `smp_store_release` → acquire/release loads/stores
- Function calls like `bintree_get_child`, `atomic_load`, etc. → may contain hidden memory operations

### 2. Identify Control Flow Structures
Analyze the function's control flow:
- **Loops**: Identify loop entry, body, condition checks, and exit points
  - Note the loop condition address
  - Identify backward jumps (loop back edges)
  - Note any loop bounds or iteration limits
- **Conditionals**: Identify branch points and their conditions
- **Early exits**: Identify break/continue/return statements

### 3. Trace Return Paths
For each possible return path:
- **Identify all return points** in the function (look for `ret` instructions or jumps to return blocks)
- **Trace backwards** from each return to the function entry
- **Document the condition** that leads to each return
- **Note the return value** (typically in `rax` register)

### 4. Create Control Flow Expressions
Using regex-like notation, express each path from entry to return:
- Use `->` for sequential execution
- Use `*` for zero or more iterations (loops)
- Use `+` for one or more iterations
- Use `|` for alternatives (branches)
- Use `()` for grouping
- Use `?` for optional operations

**Example patterns:**
- Simple path: `m00 -> m01 -> m02 -> RETURN`
- Loop: `m00 -> (m01 -> m02)* -> m03 -> RETURN`
- Conditional: `m00 -> (m01|m02) -> m03 -> RETURN`
- Complex: `m00 -> (m01 -> m02 -> (m03|m04))* -> m05 -> m06 -> RETURN`

### 5. Document Results
Create a markdown file with the following sections:

#### Header
- Function name and address range
- Brief description of the function's purpose

#### Memory Operations Section
For each memory operation:
```markdown
### mXX: Operation Description
**Location:** 0xADDR
**C Code:** `[corresponding C code]`
**Assembly:**
```asm
ADDR:  instruction   # context
ADDR:  instruction   # MEMORY LOAD/STORE/CAS
ADDR:  instruction   # context
```
```

#### Control Flow Analysis Section
- Describe loop structures with entry/exit points
- Note loop conditions and bounds
- Identify conditional branches
- Document backoff or early exit mechanisms

#### Return Paths Section
For each return path:
- Path number and description
- Return value
- Detailed trace with addresses
- Control flow expression using regex notation

#### Unified Expression Section
- Show all paths in a single view
- Highlight differences between paths
- Note any alternative branches using `|` notation

#### Notes Section
- Memory ordering semantics (acquire/release)
- Concurrency mechanisms (locks, CAS operations)
- Special handling (backoff, retry loops)
- Performance considerations
- Any invariants or constraints

## Example Invocation

```
Do the following: 
1. In this selection, identify every memory operation (load/store/CAS) based on the inlined C code hints. 
2. Identify the corresponding assembly code that performs the memory operation. Label each of these m00, m01, m02 and so on. 
3. Study the control flow paths of this function. There will be one or more loops some with conditionals, some without. 
4. Identify each return path from the function. Trace all paths from the start of the function to the return path. Use regex notation to identify loops and retries (if any). So I want something like m0->m1->(m2->m3->m4)*->m5->m6->(m7->m8)+
5. Create [FUNCTION_NAME]_ASM.md with your results.
```

## Tips

1. **Start with memory operations**: Scan the assembly first for all memory accesses
2. **Use C code hints**: The inline comments usually indicate what the code is doing
3. **Identify function calls**: Functions like `bintree_get_child` may hide memory operations inside them
4. **Check memory ordering**: Look for acquire/release patterns, especially in concurrent data structures
5. **Trace jumps carefully**: Use `jmp`, `je`, `jne`, `jle`, etc. to understand control flow
6. **Count iterations**: Note any loop counters or depth limits
7. **Look for patterns**: Retry loops, backoff mechanisms, optimistic reads
8. **Document alternatives**: When there are conditional branches, show both paths using `|`

## Common Assembly Patterns

**Loop pattern:**
```asm
  jmp   loop_check        # Jump to condition
loop_body:
  ...                     # Loop body
  inc   counter           # Increment
loop_check:
  cmp   counter, MAX      # Check condition
  jle   loop_body         # Loop back if true
  ...                     # Exit
```

**Acquire load pattern:**
```asm
  mov   rax, [addr]       # Load with acquire semantics
  # Or via function call
  call  smp_load_acquire
```

**Conditional branch pattern:**
```asm
  cmp   rax, rbx
  jl    left_branch       # if less than
  ...   right_path
  jmp   merge
left_branch:
  ...   left_path
merge:
  ...   continue
```

## Output Format

Save results to: `[FUNCTION_NAME]_ASM.md`

Use clear markdown formatting with:
- Headers (##, ###) for organization
- Code blocks with `asm` syntax highlighting
- Inline code for registers and variables
- Bold for emphasis on operation types
- Tables for summary information (optional)
