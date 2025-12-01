#!/bin/bash
# Correctness verification test for BPF Arena Data Structure Framework
# Tests data structure integrity and consistency

set -e

echo "=========================================="
echo "  BPF Arena Framework - Verification Test"
echo "=========================================="
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

# Function to run a test with verification
run_verify_test() {
    local test_name="$1"
    shift
    local cmd="$@"
    
    echo "Running: $test_name"
    echo "  Command: $cmd"
    
    if $cmd > /tmp/verify_output.txt 2>&1; then
        # Check if verification passed
        if grep -q "verification PASSED" /tmp/verify_output.txt; then
            echo "  ✓ PASSED - Verification successful"
            
            # Show element count
            grep "Elements in list" /tmp/verify_output.txt || true
            echo ""
            
            TESTS_PASSED=$((TESTS_PASSED + 1))
            return 0
        else
            echo "  ✗ FAILED - Verification failed"
            grep -A 5 "verification" /tmp/verify_output.txt || true
            echo ""
            TESTS_FAILED=$((TESTS_FAILED + 1))
            return 1
        fi
    else
        echo "  ✗ FAILED - Program crashed"
        cat /tmp/verify_output.txt | tail -20
        echo ""
        TESTS_FAILED=$((TESTS_FAILED + 1))
        return 1
    fi
}

echo "Verify Test 1: Single Thread Operations"
echo "----------------------------------------"
run_verify_test "single thread" ./skeleton -t 1 -o 500 -w mixed -v

echo "Verify Test 2: Low Concurrency"
echo "----------------------------------------"
run_verify_test "low concurrency" ./skeleton -t 2 -o 1000 -w mixed -v

echo "Verify Test 3: Medium Concurrency"
echo "----------------------------------------"
run_verify_test "medium concurrency" ./skeleton -t 4 -o 2000 -w mixed -v

echo "Verify Test 4: High Concurrency"
echo "----------------------------------------"
run_verify_test "high concurrency" ./skeleton -t 8 -o 1000 -w mixed -v

echo "Verify Test 5: Insert-Heavy with Verification"
echo "----------------------------------------------"
run_verify_test "insert heavy" ./skeleton -t 4 -o 2000 -w insert -v

echo "Verify Test 6: Small Key Space (High Contention)"
echo "--------------------------------------------------"
run_verify_test "high contention" ./skeleton -t 4 -o 1000 -k 100 -w mixed -v

echo ""
echo "=========================================="
echo "  Verification Test Summary"
echo "=========================================="
echo "  Passed: $TESTS_PASSED"
echo "  Failed: $TESTS_FAILED"
echo ""

if [ $TESTS_FAILED -eq 0 ]; then
    echo "✓ All verification tests passed!"
    echo "  Data structure integrity maintained under concurrent access"
    exit 0
else
    echo "✗ Some verification tests failed."
    echo "  This indicates potential race conditions or corruption."
    exit 1
fi
