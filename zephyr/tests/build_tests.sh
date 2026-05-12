#!/bin/bash
# Build script for common link protocol test suite
#
# Usage: ./build_tests.sh [options]
# Options:
#   --board <board>   Specify Zephyr board (default: native_sim)
#   --clean           Remove build directory before building
#   --verbose         Show full build output

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BOARD="${BOARD:-native_sim}"
CLEAN=0
VERBOSE=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --board)
            BOARD="$2"
            shift 2
            ;;
        --clean)
            CLEAN=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "Building common link protocol tests..."
echo "  Board: $BOARD"
echo "  Build dir: $BUILD_DIR"

# Set NCS environment
export ZEPHYR_BASE=/opt/nordic/ncs/v3.3.0/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/nordic/ncs/toolchains/0c0f19d91c/opt/zephyr-sdk

# Clean if requested
if [ $CLEAN -eq 1 ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Build
echo "Running CMake configuration..."
cmake -B "$BUILD_DIR" \
    -DBOARD="$BOARD" \
    -DZephyr_DIR:PATH="$ZEPHYR_BASE/cmake" \
    "$SCRIPT_DIR"

echo "Building tests..."
if [ $VERBOSE -eq 1 ]; then
    cmake --build "$BUILD_DIR" --verbose
else
    cmake --build "$BUILD_DIR"
fi

echo ""
echo "✓ Build complete!"
echo ""
echo "Test binary: $BUILD_DIR/zephyr/zephyr.elf"
echo ""
echo "To run tests:"
echo "  ctest -V --test-dir $BUILD_DIR"
echo ""
