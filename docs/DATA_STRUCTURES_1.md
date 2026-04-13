# Data Structure Performance Comparison

## Test conditions

4-core machine, 1 touch worker generating `inode_create` events, ~10s runs producing ~890 events each. Every data structure implements a two-lane relay (kernel→user KU lane, user→kernel UK lane). The User Consumer "Total" column in raw data is inflated by 5-6 orders of magnitude because the userspace relay thread spin-polls the KU queue with zero backoff; each empty-queue poll counts as a failed operation. Only Avg-OK(ns) is meaningful for User Consumer. User Consumer "Total" and "Throughput" should be disregarded.

## Comparison

| Data Structure | Type | LKMM Prod (ns) | User Cons (ns) | User Prod (ns) | LKMM Cons (ns) | E2E (ns) | Tput (Mop/s) | Relay % |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| CK FIFO SPSC | queue | 69 | 99 | 100 | 48 | 316 | 3.16 | 100% |
| Folly SPSC | queue | 129 | 99 | 59 | 59 | 346 | 2.89 | 100% |
| CK Ring SPSC | ring | 119 | 130 | 76 | 47 | 372 | 2.69 | 100% |
| KCOV Buffer | buffer | 188 | 88 | 99 | 33 | 408 | 2.45 | 100% |
| IO_URING Ring | ring | 241 | 106 | 67 | 42 | 456 | 2.19 | 100% |
| IOURING_LIBURING Ring | ring | 237 | 138 | 82 | 48 | 505 | 1.98 | 100% |
| Vyukhov MPMC | queue | 156 | 179 | 187 | 118 | 640 | 1.56 | 100% |
| CK Stack UPMC | stack | 241 | 140 | 163 | 135 | 679 | 1.47 | 100% |
| MSQueue | queue | 161 | 269 | 134 | 357 | 921 | 1.09 | 100% |

All latencies are Avg-OK(ns) except User Producer where success rate = 100% (uses Avg(ns), which equals Avg-OK(ns) in that case). E2E is the sum of all four columns. Tput is the theoretical single-pipeline throughput derived from E2E: `1e9 / E2E(ns)`. Relay % is the fraction of KU-popped items that the user producer successfully enqueued into UK.

## Key observations

- **All structures achieve 100% relay in this run.** Unlike the previous benchmark where only MSQueue, CK FIFO SPSC, and CK Stack achieved 100% relay, all nine structures now relay every item. This eliminates relay completeness as a differentiator.
- **CK FIFO SPSC achieves the lowest E2E (316ns)**, overtaking the array-based structures. Its LKMM producer is the fastest at 69ns — a single linked-list enqueue with no CAS contention in SPSC mode.
- **KCOV Buffer has the lowest LKMM consumer latency (33ns)** — a simple array index read with no pointer chasing or CAS.
- **Folly SPSC has the lowest User Producer latency (59ns)** and tied-lowest LKMM Consumer (59ns), benefiting from its bounded array design with no CAS overhead.
- **IOURING_LIBURING Ring (505ns) is close to IO_URING Ring (456ns)** — the liburing wrapper adds ~49ns of overhead, mostly in the User Consumer path (+32ns) and User Producer path (+15ns), likely from the liburing API abstraction layer.
- **MSQueue has the highest E2E (921ns)** with notably high LKMM Consumer (357ns) and User Consumer (269ns) latencies due to MPMC CAS contention on head/tail pointers.
- **Linked-list MPMC structures (MSQueue, Vyukhov, CK Stack) cluster at the bottom** (640-921ns E2E) due to CAS overhead, node allocation, and pointer chasing. SPSC and ring structures (316-505ns) avoid most of this overhead.
- **LKMM producer latency spans 3.5x** (69ns for CK FIFO SPSC to 241ns for IO_URING Ring and CK Stack UPMC), while LKMM consumer latency spans 10.8x (33ns for KCOV to 357ns for MSQueue).

## Note on User Consumer metrics

The User Consumer "Total" counts include ~153-173M empty-queue poll iterations from the busy-wait relay loop. Only the Avg-OK(ns) column (88-269ns across structures) reflects the actual cost of a successful dequeue. Throughput numbers for this category measure polling rate, not data structure performance.

## Note on E2E methodology

E2E is computed as the sum of the four per-operation Avg-OK(ns) values (LKMM Producer + User Consumer + User Producer + LKMM Consumer). This reflects the operational cost of moving a message through the full pipeline. The BPF code also records a wall-clock E2E timestamp (kernel producer to kernel consumer), but that measurement includes ~538-609μs of queue-wait time dominated by the 1ms `usleep` polling interval in the userspace relay loop, making it unsuitable for comparing data structure performance.
