#!/bin/bash
# Quick test runner for common link protocol tests
#
# This script builds and runs the test suite with verbose output

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "========================================"
echo "Common Link Protocol Test Suite Runner"
echo "========================================"
echo ""

# Set NCS environment
export ZEPHYR_BASE=${ZEPHYR_BASE:-/opt/nordic/ncs/v3.3.0/zephyr}
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-/opt/nordic/ncs/toolchains/0c0f19d91c/opt/zephyr-sdk}
export CMAKE_PREFIX_PATH="${ZEPHYR_BASE}/cmake"

if [ ! -d "$ZEPHYR_BASE" ]; then
    echo "ERROR: ZEPHYR_BASE not found at: $ZEPHYR_BASE"
    echo "Please set ZEPHYR_BASE environment variable"
    exit 1
fi

# Configure and build
echo "Step 1: Configuring build..."
cmake -B "$BUILD_DIR" \
    -DBOARD=native_sim \
    -DZephyr_DIR:PATH="$ZEPHYR_BASE/cmake" \
    "$SCRIPT_DIR" || {
    echo "ERROR: CMake configuration failed"
    exit 1
}

echo ""
echo "Step 2: Building tests..."
cmake --build "$BUILD_DIR" || {
    echo "ERROR: Build failed"
    exit 1
}

echo ""
echo "Step 3: Running tests..."
echo "========================================"
echo ""

# Run tests with verbose output
cd "$BUILD_DIR" || exit 1
ctest -V --output-on-failure

TEST_RESULT=$?

echo ""
echo "========================================"
if [ $TEST_RESULT -eq 0 ]; then
    echo "✓ All tests passed!"
else
    echo "✗ Some tests failed (exit code: $TEST_RESULT)"
fi
echo "========================================"

exit $TEST_RESULT
