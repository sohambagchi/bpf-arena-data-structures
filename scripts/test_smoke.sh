#!/bin/bash
# Basic smoke test for BPF Arena Data Structure Framework
# Tests basic functionality with minimal operations
#
# NOTE: This script is a TEMPLATE for a multi-threaded implementation.
# The current skeleton implementation uses a simpler design:
# - Kernel: LSM hook inserts on file creation
# - Userspace: Single-threaded reader (sleeps, then reads)
# 
# Command-line options like -t, -o, -w are for future multi-threaded versions.
# To test the current implementation, use:
#   sudo ./skeleton -d 5
#   sudo ./skeleton_msqueue -d 5

set -e

echo "=========================================="
echo "  BPF Arena Framework - Smoke Test"
echo "=========================================="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: This script must be run as root (use sudo)"
    exit 1
fi

# Check if programs exist
if [ ! -f "./skeleton" ]; then
    echo "ERROR: skeleton program not found. Run 'make' first."
    exit 1
fi

if [ ! -f "./arena_list" ]; then
    echo "ERROR: arena_list program not found. Run 'make' first."
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
    
    if $cmd > /tmp/test_output.txt 2>&1; then
        echo "  ✓ PASSED"
        echo ""
        TESTS_PASSED=$((TESTS_PASSED + 1))
        return 0
    else
        echo "  ✗ FAILED"
        echo "  Output:"
        cat /tmp/test_output.txt | head -20
        echo ""
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

echo "Test 1: Arena List Example (10 elements)"
echo "------------------------------------------"
run_test "arena_list basic" ./arena_list 10

echo "Test 2: Skeleton - Single Thread Insert"
echo "------------------------------------------"
run_test "skeleton single thread" ./skeleton -t 1 -o 100 -w insert

echo "Test 3: Skeleton - Multiple Threads Mixed"
echo "------------------------------------------"
run_test "skeleton multi-thread" ./skeleton -t 4 -o 200 -w mixed

echo "Test 4: Skeleton - Search Workload"
echo "------------------------------------------"
run_test "skeleton search" ./skeleton -t 2 -o 150 -w search

echo "Test 5: Skeleton - Delete Workload"
echo "------------------------------------------"
run_test "skeleton delete" ./skeleton -t 2 -o 100 -w delete

echo ""
echo "=========================================="
echo "  Test Summary"
echo "=========================================="
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo "✓ All smoke tests passed!"
    exit 0
else
    echo "✗ Some tests failed. Check output above."
    exit 1
fi
