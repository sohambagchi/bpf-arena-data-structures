#!/bin/bash

set -e

echo "=========================================="
echo "  BPF Arena Framework - Performance Benchmark"
echo "=========================================="
echo ""

if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root (use sudo)"
    exit 1
fi

if [ ! -f "./skeleton" ]; then
    echo "ERROR: skeleton program not found. Run 'make' first."
    exit 1
fi

RESULTS_FILE="benchmark_results.txt"
echo "Benchmark Results - $(date)" > $RESULTS_FILE
echo "=====================================" >> $RESULTS_FILE
echo "" >> $RESULTS_FILE

run_benchmark() {
    local test_name="$1"
    shift
    local cmd="$@"
    
    echo "Benchmark: $test_name"
    echo "  Command: $cmd"
    
    if $cmd > /tmp/bench_output.txt 2>&1; then
        local ops_per_sec=$(grep "^TOTAL" /tmp/bench_output.txt | awk '{print $NF}')
        local total_ops=$(grep "^TOTAL" /tmp/bench_output.txt | awk '{print $2}')
        
        echo "  Operations: $total_ops"
        echo "  Throughput: $ops_per_sec ops/sec"
        echo ""
        
        echo "$test_name" >> $RESULTS_FILE
        echo "  Throughput: $ops_per_sec ops/sec" >> $RESULTS_FILE
        echo "  Total Ops:  $total_ops" >> $RESULTS_FILE
        echo "" >> $RESULTS_FILE
        
        return 0
    else
        echo "  ✗ FAILED"
        echo ""
        return 1
    fi
}

echo "=== Thread Scaling Tests ==="
echo ""
for threads in 1 2 4 8 16; do
    run_benchmark "Scaling: $threads threads" ./skeleton -t $threads -o 5000 -w mixed
done

echo "=== Workload Type Tests ==="
echo ""
run_benchmark "Workload: Insert only" ./skeleton -t 4 -o 5000 -w insert
run_benchmark "Workload: Search only" ./skeleton -t 4 -o 5000 -w search
run_benchmark "Workload: Delete only" ./skeleton -t 4 -o 5000 -w delete
run_benchmark "Workload: Mixed" ./skeleton -t 4 -o 5000 -w mixed

echo "=== Contention Tests ==="
echo ""
run_benchmark "Contention: Low (100000 keys)" ./skeleton -t 4 -o 5000 -k 100000 -w mixed
run_benchmark "Contention: Medium (10000 keys)" ./skeleton -t 4 -o 5000 -k 10000 -w mixed
run_benchmark "Contention: High (1000 keys)" ./skeleton -t 4 -o 5000 -k 1000 -w mixed
run_benchmark "Contention: Very High (100 keys)" ./skeleton -t 4 -o 5000 -k 100 -w mixed

echo ""
echo "=========================================="
echo "  Benchmark Complete"
echo "=========================================="
echo ""
echo "Results saved to: $RESULTS_FILE"
echo ""
cat $RESULTS_FILE
