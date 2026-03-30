# Data Structure Performance Comparison

## Test conditions

4-core machine, 1 touch worker generating `inode_create` events, ~10s runs producing ~890 events each. Every data structure implements a two-lane relay (kernel→user KU lane, user→kernel UK lane). The User Consumer "Total" column in raw data is inflated by 5-6 orders of magnitude because the userspace relay thread spin-polls the KU queue with zero backoff; each empty-queue poll counts as a failed operation. Only Avg-OK(ns) is meaningful for User Consumer. User Consumer "Total" and "Throughput" should be disregarded.

## Comparison

| Data Structure | Type | LKMM Prod (ns) | User Cons (ns) | User Prod (ns) | LKMM Cons (ns) | E2E (ns) | Relay % |
|---|---|---:|---:|---:|---:|---:|---:|
| KCOV Buffer | buffer | 46 | 86 | 105 | 22 | 259 | 28.4% |
| CK Ring SPSC | ring | 37 | 102 | 97 | 24 | 260 | 14.2% |
| Folly SPSC | queue | 77 | 114 | 87 | 22 | 300 | 14.2% |
| IO_URING Ring | ring | 56 | 109 | 131 | 22 | 318 | 14.3% |
| CK FIFO SPSC | queue | 63 | 126 | 137 | 24 | 350 | 100% |
| CK Stack UPMC | stack | 181 | 120 | 133 | 36 | 470 | 100% |
| Vyukhov | queue | 125 | 146 | 210 | 32 | 513 | 14.4% |
| MSQueue | queue | 122 | 226 | 191 | 118 | 657 | 100% |

All latencies are Avg-OK(ns) except User Producer where success rate = 100% (uses Avg(ns), which equals Avg-OK(ns) in that case). E2E is the sum of all four columns. Relay % is the fraction of KU-popped items that the user producer successfully enqueued into UK.

## Key observations

- **Array-based structures dominate.** KCOV Buffer (259ns) and CK Ring SPSC (260ns) achieve the lowest end-to-end latency. Both write to pre-allocated array slots with no pointer chasing or CAS retry loops.
- **Relay completeness splits into two groups.** MSQueue, CK FIFO SPSC, and CK Stack UPMC relay 100% of items. The remaining five structures drop 71-86% of items because their bounded UK capacity fills up faster than the kernel consumer drains it.
- **CK Ring SPSC has the lowest LKMM producer latency (37ns)** — a single array write with a store-release on the producer index.
- **CK Stack UPMC has the highest LKMM producer latency (181ns)** — CAS on the stack head pointer under potential contention.
- **MSQueue has notably high User Producer latency (191ns)** — MPMC CAS contention on the tail pointer during enqueue.
- **Linked-list structures (MSQueue, Vyukhov, CK Stack) have higher kernel producer latency** (122-181ns) due to node allocation, pointer chasing, and CAS overhead compared to array-based structures (37-77ns).
- **SPSC and ring structures (Folly, CK Ring, IO_URING, KCOV) achieve the lowest LKMM consumer latencies (~22ns)**, benefiting from no CAS contention — a single load-acquire on the consumer index suffices.

## Note on User Consumer metrics

The User Consumer "Total" counts include ~152-177M empty-queue poll iterations from the busy-wait relay loop. Only the Avg-OK(ns) column (86-226ns across structures) reflects the actual cost of a successful dequeue. Throughput numbers for this category measure polling rate, not data structure performance.
