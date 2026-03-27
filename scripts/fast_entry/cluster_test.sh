#!/bin/bash

TOP_DIR=$(pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL"
BUILD_DIR="$SIM_MODEL_DIR/build/cluster_test"
CMAKEFILE_DIR="$SIM_MODEL_DIR/test"

if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake -DENABLE_DEBUG_UTILS=OFF $CMAKEFILE_DIR
make test_sram -j8
./test_sram
cd ..
