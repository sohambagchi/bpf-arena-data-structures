Here is the implementation specification for the **SPSC (Single-Producer Single-Consumer) Ring Buffer**, adapting the Folly `ProducerConsumerQueue` algorithm to the BPF Arena framework.

# Implementation Specification: SPSC Ring Buffer

## 1. Introduction

This document specifies the implementation of a lock-free, wait-free Single-Producer Single-Consumer (SPSC) queue. The algorithm is derived directly from Folly's `ProducerConsumerQueue`. It utilizes a fixed-size ring buffer with atomic indices to manage concurrent access between a kernel-space BPF program (Producer) and a user-space application (Consumer), or vice versa.

**Key Characteristics:**

* 
**Lock-Free:** Uses C11 atomic primitives with explicit memory ordering (`acquire`/`release`) instead of mutexes.


* **Wait-Free:** Operations complete in a finite number of steps without blocking.
* **Dual Context:** Designed for high-throughput telemetry where the BPF program enqueues events (Producer) and userspace consumes them (Consumer).
* 
**Cache Friendly:** Explicit padding prevents false sharing between the read and write indices.



## 2. Data Structure Organization

The data structure consists of a control header and a contiguous data array allocated in the BPF arena.

### 2.1 Arena Memory Layout

```c
#define CACHE_LINE_SIZE 64

struct ds_spsc_node {
    __u64 key;
    __u64 value;
};

struct ds_spsc_queue_head {
    // Producer writes to write_idx, Consumer reads it
    // Aligned to isolate cache line
    struct {
        __u32 idx;
    } write_idx __attribute__((aligned(CACHE_LINE_SIZE)));

    // Consumer writes to read_idx, Producer reads it
    struct {
        __u32 idx;
    } read_idx __attribute__((aligned(CACHE_LINE_SIZE)));

    __u32 size;             // Total number of slots (capacity + 1)
    struct ds_spsc_node __arena *records; // Pointer to contiguous array
};

```

### 2.2 Invariants

1. 
**Size Constraint:** `size` must be .


2. **Usable Capacity:** The maximum number of elements is `size - 1`. One slot is always kept empty to distinguish between "full" and "empty" states.


3. **Indices:**  and .
4. 
**Empty Condition:** `read_idx == write_idx`.


5. 
**Full Condition:** `(write_idx + 1) % size == read_idx`.



## 3. Algorithm Pseudo-Code

### 3.1 Initialization

Allocates the fixed-size array. Note that `records_` in the original source is allocated via `malloc`; here we use `bpf_arena_alloc`.

```c
int ds_spsc_init(struct ds_spsc_queue_head __arena *head, __u32 size) {
    cast_kern(head);
    if (size < 2) return DS_ERROR_INVALID;

    // Allocate contiguous array for records
    // Note: implementation depends on arena allocator supporting size * sizeof(node)
    head->records = bpf_arena_alloc(size * sizeof(struct ds_spsc_node));
    if (!head->records) return DS_ERROR_NOMEM;
    
    // Explicitly zero memory (optional if allocator zeros)
    // cast_kern(head->records);
    
    head->size = size;
    head->read_idx.idx = 0;
    head->write_idx.idx = 0; [cite_start]// [cite: 8]
    
    return DS_SUCCESS;
}

```

### 3.2 Insert (Producer)

Maps to `write` in Folly. The producer loads the `read_idx` with `ACQUIRE` semantics to check for space.

```c
int ds_spsc_insert(struct ds_spsc_queue_head __arena *head, __u64 key, __u64 value) {
    cast_kern(head);
    
    // Load write index (Relaxed: only this thread writes to it)
    __u32 current_write = READ_ONCE(head->write_idx.idx); [cite_start]// [cite: 15]
    
    // Calculate next write index
    __u32 next_record = current_write + 1;
    if (next_record >= head->size) {
        next_record = 0; [cite_start]// [cite: 16]
    }

    // Check against read index (Acquire: synchronize with Consumer)
    __u32 current_read = smp_load_acquire(&head->read_idx.idx); [cite_start]// [cite: 17]
    
    if (next_record != current_read) {
        // Space available. Perform the write.
        // Direct pointer arithmetic on arena pointer
        struct ds_spsc_node __arena *node = &head->records[current_write];
        cast_kern(node);
        
        node->key = key;
        node->value = value; [cite_start]// [cite: 17]

        // Commit the write (Release: make payload visible before index update)
        smp_store_release(&head->write_idx.idx, next_record); [cite_start]// [cite: 17]
        return DS_SUCCESS;
    }

    // Queue is full
    return DS_ERROR_FULL; [cite_start]// [cite: 18]
}

```

### 3.3 Delete (Consumer)

Maps to `read` or `popFront`. The consumer loads the `write_idx` with `ACQUIRE` semantics to check for data.
*Note: The standard `delete(key)` API is adapted here to be `dequeue()`. The `key` argument is ignored.*

```c
int ds_spsc_delete(struct ds_spsc_queue_head __arena *head, __u64 *val_out) {
    cast_kern(head);

    // Load read index (Relaxed: only this thread writes to it)
    __u32 current_read = READ_ONCE(head->read_idx.idx); [cite_start]// [cite: 19]
    
    // Check against write index (Acquire: synchronize with Producer)
    __u32 current_write = smp_load_acquire(&head->write_idx.idx); [cite_start]// [cite: 20]

    if (current_read == current_write) {
        return DS_ERROR_NOT_FOUND; [cite_start]// Queue is empty [cite: 20]
    }

    // Retrieve data
    struct ds_spsc_node __arena *node = &head->records[current_read];
    cast_kern(node);
    if (val_out) *val_out = node->value; [cite_start]// [cite: 22]

    // Calculate next read index
    __u32 next_record = current_read + 1;
    if (next_record >= head->size) {
        next_record = 0; [cite_start]// [cite: 21]
    }

    // Commit the read (Release: signal slot is free)
    smp_store_release(&head->read_idx.idx, next_record); [cite_start]// [cite: 23]
    return DS_SUCCESS;
}

```

## 4. Concurrency & Memory Safety

### 4.1 Memory Ordering

This algorithm relies on strict memory ordering to ensure data validity without locks:

* **Producer:** Writes data  `STORE-RELEASE` `write_idx`.
* **Consumer:** `LOAD-ACQUIRE` `write_idx`  Reads data.
* **Safety:** The consumer cannot observe the new `write_idx` until the data write is globally visible.
* **Reverse:** Consumer reads data  `STORE-RELEASE` `read_idx`. Producer `LOAD-ACQUIRE` `read_idx`.
* **Safety:** The producer cannot overwrite a slot until the consumer has finished reading and updated the index.

### 4.2 Single Writer Constraint

The `ProducerConsumerQueue` explicitly assumes **one** producer and **one** consumer.

* If multiple BPF programs try to `insert` concurrently, they will race on `current_write` and overwrite each other's data.
* **Mitigation:** This structure should be used with a `spin_lock` if multiple producers exist, or strictly limited to per-CPU arrays where only one CPU accesses the queue.

### 4.3 False Sharing

The source implementation explicitly adds padding around the atomic indices:


`char pad0_[hardware_destructive_interference_size];` `alignas(...) AtomicIndex readIndex_;` 
In our struct, we use `__attribute__((aligned(64)))` to ensure `read_idx` and `write_idx` reside on different cache lines, preventing cache thrashing between the producer (writing `write_idx`) and consumer (writing `read_idx`).

## 5. Implementation Considerations

### 5.1 API Mapping Deviations

The standard `ds_api` defines `delete(key)`.

* **Deviation:** This implementation ignores `key` in `delete`. It operates purely as FIFO `popFront`.
* **Search:** `ds_search` is . It requires iterating from `read_idx` to `write_idx`.

### 5.2 Verification Approach

The `verify` function checks the consistency of indices.

```c
int ds_spsc_verify(struct ds_spsc_queue_head __arena *head) {
    cast_kern(head);
    __u32 r = READ_ONCE(head->read_idx.idx);
    __u32 w = READ_ONCE(head->write_idx.idx);
    __u32 s = head->size;

    if (r >= s || w >= s) return DS_ERROR_CORRUPT;
    
    // Recalculate size guess
    int size_guess = (int)w - (int)r;
    if (size_guess < 0) size_guess += s; [cite_start]// [cite: 37]
    
    if (size_guess > (s - 1)) return DS_ERROR_CORRUPT;
    
    return DS_SUCCESS;
}

```

## 6. Example Concurrent Scenario

**Initial State:** `size=10`, `read=0`, `write=0`.

1. **Producer (BPF):** Calls `insert(k1, v1)`.
* Loads `write(0)`, `read(0)`.
* `next = 1`. `1 != 0`.
* Writes `records[0] = {k1, v1}`.
* `store_release(write, 1)`.


2. **Consumer (User):** Calls `delete()`.
* Loads `read(0)`.
* `load_acquire(write)` sees `1`.
* Reads `records[0]`.
* `store_release(read, 1)`.


3. **Race Condition (Full):**
* Producer fills queue until `write=9`, `read=0`.
* Producer calls `insert`. `next = (9+1)%10 = 0`.
* Checks `read(0)`. `next == read`. Returns `DS_ERROR_FULL`.
* Producer must back off or drop event.



## 7. Implementation Status

* 
**Core Logic:** Ported from Folly  including wrap-around logic.


* **Memory Safety:** Arena pointers and cast calls integrated.
* **Limitation:** Strictly SPSC. If used in a context with multiple producers (e.g., multiple BPF program triggers on the same queue), external synchronization is required.