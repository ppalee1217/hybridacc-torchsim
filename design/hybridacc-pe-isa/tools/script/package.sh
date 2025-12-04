#!/bin/bash

ROOT_DIR=$(pwd)
PACKAGER=$ROOT_DIR/tools/bin/ha-package

# Define the directory to package
SOURCE_DIR="/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-pe-isa/asm/template"
OUTPUT_DIR="/home/yoyo/work/MasterResearch/HybridAcc/design/hybridacc-pe-isa/output/package"

# Define package name and output files
PACKAGE_NAME="kernel"
PKG_BINARY="$OUTPUT_DIR/${PACKAGE_NAME}.pkg"
PKG_JSON="$OUTPUT_DIR/${PACKAGE_NAME}.json"
PKG_CHEADER="$OUTPUT_DIR/${PACKAGE_NAME}.h"

mkdir -p "$OUTPUT_DIR"

# source files to be packaged
SOURCE_FILES="$SOURCE_DIR"/*

# Run ha-package to package the directory
echo "=========================================="
echo "Running ha-package with the following parameters:"
echo "  Packager: $PACKAGER"
echo "  Output Binary: $PKG_BINARY"
echo "  Output JSON: $PKG_JSON"
echo "  Output Header: $PKG_CHEADER"
echo "  Source Files: $SOURCE_FILES"
echo "=========================================="
echo ""

$PACKAGER \
    -o "$PKG_BINARY" \
    --json "$PKG_JSON" \
    --header "$PKG_CHEADER" \
    --verbose \
    $SOURCE_FILES

EXIT_CODE=$?

echo ""
echo "=========================================="
# Check if the command succeeded
if [ $EXIT_CODE -eq 0 ]; then
    echo "Packaging completed successfully."
else
    echo "Packaging failed with exit code: $EXIT_CODE"
    exit 1
fi
echo "=========================================="