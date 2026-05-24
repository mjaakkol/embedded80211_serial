#!/bin/bash
# Quick test runner for common link protocol tests
#
# This script builds and runs the test suite with verbose output

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BOARD="${BOARD:-qemu_cortex_m3}"

echo "========================================"
echo "Common Link Protocol Test Suite Runner"
echo "========================================"
echo ""

# Set NCS environment
export ZEPHYR_BASE=${ZEPHYR_BASE:-/opt/nordic/ncs/v3.3.0/zephyr}
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-/opt/nordic/ncs/toolchains/0c0f19d91c/opt/zephyr-sdk}
export ZEPHYR_TOOLCHAIN_VARIANT=${ZEPHYR_TOOLCHAIN_VARIANT:-zephyr}

if [ ! -d "$ZEPHYR_BASE" ]; then
    echo "ERROR: ZEPHYR_BASE not found at: $ZEPHYR_BASE"
    echo "Please set ZEPHYR_BASE environment variable"
    exit 1
fi

if ! command -v west >/dev/null 2>&1; then
    echo "ERROR: west not found in PATH"
    exit 1
fi

# Configure and build
echo "Step 1: Building tests for board: $BOARD"
west build -p always -b "$BOARD" "$SCRIPT_DIR" || {
    echo "ERROR: west build failed"
    exit 1
}

echo ""
echo "Step 2: Running tests..."
cd "$BUILD_DIR/tests" || exit 1
west build -t run || {
    echo "ERROR: Test run failed"
    exit 1
}

echo ""
echo "========================================"
echo "✓ All tests passed!"
echo "========================================"

exit 0
