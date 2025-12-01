# Implementation Specification: The Michael-Scott Concurrent Queue

This document serves as a comprehensive design specification for an AI Agent or developer to implement the concurrent queue algorithms described in the 1996 paper *Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms* by Maged M. Michael and Michael L. Scott.

-----

## 1\. Introduction

[cite\_start]This specification details two variations of a First-In-First-Out (FIFO) queue data structure designed for high-performance multiprocessor systems[cite: 8]. [cite\_start]The primary focus is the **Non-Blocking (Lock-Free)** algorithm, which allows concurrent access without preventing faster processes from completing operations if a slower process is delayed[cite: 23, 26]. [cite\_start]A secondary **Two-Lock** algorithm is described for systems lacking the necessary atomic primitives for the non-blocking version[cite: 13, 100].

**Key Concepts:**

  * [cite\_start]**Safety:** The algorithms ensure the linked list remains connected, nodes are inserted only at the end, and deleted only from the beginning[cite: 106, 108].
  * [cite\_start]**Liveness:** The non-blocking version guarantees that some operation will complete within a finite number of steps (system-wide progress)[cite: 23].
  * [cite\_start]**Practicality:** Both algorithms are designed to be simple and fast, outperforming standard single-lock queues in high-contention or multiprogrammed environments[cite: 9, 10].

-----

## 2\. Internal Operations: Data Structure Organization

### 2.1 The Underlying Structure

[cite\_start]Both algorithms utilize a **singly-linked list** with two explicit pointers: `Head` and `Tail`[cite: 89, 101].

  * **The Dummy Node:** A critical structural invariant is that the queue *always* contains at least one node. [cite\_start]The `Head` pointer points to a "dummy" node (the node that was most recently dequeued or the initial placeholder), not the actual first valid data element[cite: 90, 102]. The actual data starts at `Head.next`.
  * [cite\_start]**Separation of Concerns:** This dummy node creates a buffer between the `Head` and `Tail`, effectively decoupling enqueue and dequeue operations so they do not access the same memory location when the queue is not empty (or near-empty)[cite: 103].

### 2.2 Metadata and Node Anatomy

#### Node Structure

Each node in the list contains:

1.  [cite\_start]`value`: The data stored in the queue[cite: 125].
2.  [cite\_start]`next`: A pointer to the next node in the list[cite: 125].

#### Non-Blocking Queue Metadata

[cite\_start]To support lock-free operations and prevent the **ABA Problem** (where a pointer is reused, leading a thread to believe the queue state hasn't changed when it actually has), pointers must be associated with a **Modification Counter**[cite: 92].

  * **Pointer Structure:** A pointer is not just a memory address. [cite\_start]It is a tuple: `<memory_address, count>`[cite: 124].
  * [cite\_start]**Atomic Primitive:** The implementation requires a `Compare_And_Swap` (CAS) operation that can atomically update this tuple (or a double-width CAS if the language requires it)[cite: 92, 74].

#### Two-Lock Queue Metadata

The locking version does not require modification counters but requires two mutual exclusion locks:

1.  [cite\_start]`H_lock`: Protects the `Head` pointer[cite: 238].
2.  [cite\_start]`T_lock`: Protects the `Tail` pointer[cite: 238].

-----

## 3\. Algorithms: Internal Logic

The following pseudo-code is language-agnostic. For the **Non-Blocking** implementation, assume all pointer manipulations use CAS (Compare-And-Swap).

### 3.1 Primitives Required

  * `CAS(address, expected_value, new_value)`: Atomically checks if `address` holds `expected_value`. If yes, writes `new_value` and returns `TRUE`. If no, returns `FALSE`.
  * `Load(address)`: Atomically reads the value.

### 3.2 Algorithm A: Non-Blocking (Lock-Free) Implementation

*Best for: Systems supporting CAS, high concurrency, multiprogramming.*

#### Initialization

[cite\_start]Create a dummy node to establish the invariant [cite: 127-130].

```text
function Initialize():
    node = AllocateNode()
    node.next = NULL
    Head = node
    Tail = node
```

#### Enqueue (Add)

This operation appends a value to the tail. [cite\_start]It handles a unique "lazy tail" scenario where the `Tail` pointer might lag behind the actual end of the list, requiring the enqueuer to "help" update it[cite: 297].

```text
function Enqueue(value):
    node = AllocateNode()
    node.value = value
    node.next = NULL

    loop:
        last = Tail                // Read Tail ptr & count
        next = last.next           // Read Tail.next ptr & count

        [cite_start]// Consistency Check: ensure Tail didn't change between reads [cite: 156]
        if last == Tail:
            if next.ptr == NULL:
                // Case 1: Tail is pointing to the actual last node.
                // Try to link the new node to the end.
                if CAS(&last.next, next, <node, next.count + 1>):
                    // Success! Now try to swing Tail to the new node.
                    // (If this fails, another thread will help fix it later)
                    [cite_start]CAS(&Tail, last, <node, last.count + 1>) [cite: 177]
                    return TRUE
            else:
                // Case 2: Tail is not pointing to the last node.
                // It is lagging behind. [cite_start]Help advance it. [cite: 168]
                CAS(&Tail, last, <next.ptr, last.count + 1>)
```

#### Dequeue (Access & Delete)

[cite\_start]Removes the node pointed to by `Head.next`[cite: 194].

```text
function Dequeue():
    loop:
        first = Head
        last = Tail
        next = first.next

        [cite_start]// Consistency Check [cite: 189]
        if first == Head:
            // Check for Empty Queue or Lagging Tail
            if first.ptr == last.ptr:
                if next.ptr == NULL:
                    [cite_start]// Queue is truly empty [cite: 197]
                    return EMPTY_ERROR
                else:
                    // Queue is not empty, but Tail is lagging behind Head.
                    [cite_start]// Help advance Tail before proceeding. [cite: 199]
                    CAS(&Tail, last, <next.ptr, last.count + 1>)
            else:
                // Standard Dequeue
                [cite_start]// Read value BEFORE CAS to ensure safety [cite: 214]
                val = next.value
                // Try to swing Head to the next node
                if CAS(&Head, first, <next.ptr, first.count + 1>):
                    [cite_start]Free(first.ptr) // Safe to reclaim the OLD dummy node [cite: 230]
                    return val
```

### 3.3 Algorithm B: Two-Lock Implementation

*Best for: Systems without atomic CAS, dedicated hardware.*

#### Enqueue (Two-Lock)

[cite\_start]Acquires the tail lock, adds the node, updates tail, releases lock[cite: 245].

```text
function Enqueue(value):
    node = AllocateNode()
    node.value = value
    node.next = NULL

    Acquire(T_lock)
        // Critical Section
        Tail.next = node
        Tail = node
    Release(T_lock)
```

#### Dequeue (Two-Lock)

Acquires the head lock, updates head, releases lock. [cite\_start]Uses the dummy node to avoid locking `T_lock` unless the queue is accessed[cite: 259].

```text
function Dequeue():
    Acquire(H_lock)
        node = Head
        new_head = node.next
        if new_head == NULL:
            Release(H_lock)
            return EMPTY_ERROR
        
        val = new_head.value
        Head = new_head
    Release(H_lock)
    
    Free(node) // Free the old dummy node
    return val
```

-----

## 4\. Example Flow: Non-Blocking Scenario

This section illustrates a concurrent scenario where **Thread A** attempts to Enqueue `Val_A` and **Thread B** attempts to Dequeue simultaneously.

### Initial State

The queue contains one item (Value 10). `Head` points to Dummy. `Tail` points to Node 10.

  * `Head` -\> [Dummy] -\> [Node 10] -\> NULL
  * `Tail` -\> [Node 10]

### Step 1: Thread A (Enqueue) Reads State

Thread A wants to Enqueue `Val_A`.

1.  Thread A reads `last` (points to Node 10).
2.  Thread A reads `next` (points to NULL).
3.  Thread A prepares new node `[Node A]`.

### Step 2: Thread B (Dequeue) Interrupts & Completes

Thread B wakes up and runs `Dequeue`.

1.  Thread B reads `first` (Dummy) and `next` (Node 10).
2.  Thread B performs CAS on `Head`.
      * **Pre-CAS:** `Head` -\> [Dummy] -\> [Node 10]
      * **Post-CAS:** `Head` -\> [Node 10] -\> NULL
3.  Thread B returns `10`. The old Dummy is freed. [cite\_start]`Node 10` is effectively the new Dummy[cite: 90].

### Step 3: Thread A Resumes (Failure & Retry)

Thread A attempts its CAS to link `[Node A]` to `last.next` (Node 10's next pointer).

1.  **Check:** Thread A checks if `last` is still `Tail`.
      * Depending on the exact timing, `Tail` might still be `Node 10` (Thread B updated Head, not Tail).
2.  **CAS Execution:** Thread A performs `CAS(&Node10.next, NULL, NodeA)`.
      * This **Succeeds**. `Node 10` now points to `Node A`.
3.  **Finalize:** Thread A performs `CAS(&Tail, Node10, NodeA)`.
      * `Tail` moves to `[Node A]`.

### Visualizing the Lagging Tail (The "Helper" Logic)

If Thread A succeeded in linking `[Node A]` (Step 3.2) but **crashed** before Step 3.3 (updating Tail), the queue looks like this:

  * `Head` -\> [Node 10] -\> [Node A] -\> NULL
  * `Tail` -\> [Node 10]

**Correction:** The next thread to enter `Enqueue` or `Dequeue` will detect that `Tail.next` is NOT NULL (see Logic Block `Case 2` in section 3.2). [cite\_start]That thread will perform a CAS to advance `Tail` to `[Node A]` *before* doing its own work[cite: 52, 168].

-----

## 5\. Implementation Considerations for AI Agents

When generating code based on this specification, the Agent must adhere to these specific constraints:

1.  **The ABA Problem & Language Choice:**

      * [cite\_start]**Unmanaged Languages (C/C++/Rust):** You **MUST** implement the Modification Counters (versioning) on the pointers as described in Section 2.2[cite: 71, 92]. Without this, if a node is freed and its memory address reused, the CAS will return a false positive.
      * **Managed Languages (Java/C\#/Python):** The ABA problem is less critical regarding memory addresses due to Garbage Collection (GC), but logic errors can still occur. However, standard Java `AtomicReference` is usually sufficient. If the API requires strict adherence to the paper, use a wrapper class containing `{node, counter}`.

2.  **Memory Reclamation:**

      * The paper mentions `free(node)`. [cite\_start]In non-GC languages, you cannot simply `free` a node immediately after a Dequeue CAS because other threads might still be reading that node in their local loops[cite: 66].
      * [cite\_start]**Requirement:** Implement a mechanism like **Hazard Pointers** or **Epoch Based Reclamation**, or explicitly note that the implementation relies on a Garbage Collector[cite: 54, 57].

3.  **The "Dummy" Node Invariant:**

      * Never Initialize `Head` or `Tail` to NULL. [cite\_start]They must always reference a valid object[cite: 130].

4.  **Atomic Primitives:**

      * The implementation relies on `CAS`. If the target language does not support CAS (e.g., standard Python without C-extensions), the Non-Blocking algorithm cannot be implemented faithfully. [cite\_start]In such cases, the Agent should default to the **Two-Lock** algorithm described in Section 3.3[cite: 12].