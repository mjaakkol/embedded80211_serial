#!/bin/bash
# Quick test runner for common link protocol tests
#
# This script builds and runs the test suite with verbose output

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
# 64-bit native simulator: runs on the host without QEMU or 32-bit glibc
# headers. Override with BOARD=qemu_cortex_m3 etc. if preferred.
BOARD="${BOARD:-native_sim/native/64}"

echo "========================================"
echo "Common Link Protocol Test Suite Runner"
echo "========================================"
echo ""

# Set NCS environment (sourcing /home/mikko/ncs/env.sh overrides these)
export ZEPHYR_BASE=${ZEPHYR_BASE:-/home/mikko/ncs/v3.4.1/zephyr}
export ZEPHYR_SDK_INSTALL_DIR=${ZEPHYR_SDK_INSTALL_DIR:-/home/mikko/ncs/toolchains/8285d8ad56/opt/zephyr-sdk}
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
west build -p always -d "$BUILD_DIR" -b "$BOARD" "$SCRIPT_DIR" || {
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
