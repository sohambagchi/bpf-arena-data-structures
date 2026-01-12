## bintree_get_ptr (0x1452-0x1463)
Extracts the pointer portion from a tagged update value.

## Memory Operations
- No data-structure memory operations. This function only masks a register value; stack accesses are omitted.

## Control Flow Analysis
- Straight-line code, no branches or loops.

## Return Paths
1. **Return masked pointer:** always returns `update & UPDATE_MASK_PTR`.
   - Expression: `RETURN(ptr_bits)`

## Unified Expression
`RETURN(ptr_bits)`

## Notes
- Used to recover the Info pointer from tagged update fields before CAS operations.
