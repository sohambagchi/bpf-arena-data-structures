#!/bin/bash
# Stress test for BPF Arena Data Structure Framework
# Tests with high concurrency and many operations
#
# NOTE: This script is a TEMPLATE for a multi-threaded implementation.
# The current skeleton implementation uses a simpler design:
# - Kernel: LSM hook inserts on file creation
# - Userspace: Single-threaded reader (sleeps, then reads)
# 
# Command-line options like -t, -o, -w are for future multi-threaded versions.
# To test the current implementation, use:
#   sudo ./skeleton -d 30
#   sudo ./skeleton_msqueue -d 30

set -e

echo "=========================================="
echo "  BPF Arena Framework - Stress Test"
echo "=========================================="
echo ""
echo "WARNING: This test may take several minutes"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root (use sudo)"
    exit 1
fi

# Check if program exists
if [ ! -f "./skeleton" ]; then
    echo "ERROR: skeleton program not found. Run 'make' first."
    exit 1
fi

TESTS_PASSED=0
TESTS_FAILED=0

# Function to run a test
run_test() {
    local test_name="$1"
    shift
    local cmd="$@"
    
    echo "Running: $test_name"
    echo "  Command: $cmd"
    echo "  Started at: $(date '+%H:%M:%S')"
    
    if timeout 300 $cmd > /tmp/stress_output.txt 2>&1; then
        echo "  Completed at: $(date '+%H:%M:%S')"
        echo "  ✓ PASSED"
        
        # Extract and show key metrics
        grep -E "(TOTAL|Test Duration)" /tmp/stress_output.txt || true
        echo ""
        
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo "  ✗ FAILED"
        echo "  Output:"
        cat /tmp/stress_output.txt | tail -30
        echo ""
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

echo "Stress Test 1: High Concurrency"
echo "--------------------------------"
echo "16 threads, 10000 operations each"
run_test "high concurrency" ./skeleton -t 16 -o 10000 -w mixed

echo "Stress Test 2: Large Key Space"
echo "--------------------------------"
echo "8 threads, 5000 operations, 100000 key range"
run_test "large key space" ./skeleton -t 8 -o 5000 -k 100000 -w mixed

echo "Stress Test 3: Insert Heavy"
echo "--------------------------------"
echo "8 threads, 8000 insert operations"
run_test "insert heavy" ./skeleton -t 8 -o 8000 -w insert

echo "Stress Test 4: Mixed with Small Key Space"
echo "-------------------------------------------"
echo "12 threads, 5000 operations, 100 key range (high contention)"
run_test "high contention" ./skeleton -t 12 -o 5000 -k 100 -w mixed

echo "Stress Test 5: Many Short Bursts"
echo "----------------------------------"
for i in {1..5}; do
    echo "  Burst $i/5"
    if ! ./skeleton -t 8 -o 1000 -w mixed > /tmp/burst_$i.txt 2>&1; then
        echo "  ✗ Burst $i failed"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    else
        TESTS_PASSED=$((TESTS_PASSED + 1))
    fi
done
echo "  ✓ All bursts completed"
echo ""

echo ""
echo "=========================================="
echo "  Stress Test Summary"
echo "=========================================="
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo "✓ All stress tests passed!"
    exit 0
else
    echo "✗ Some tests failed. Check output above."
    exit 1
fi
