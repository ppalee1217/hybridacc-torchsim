ROOT_DIR=$(pwd)
BUILD_DIR=$ROOT_DIR/build
SIMULATOR=$ROOT_DIR/tools/bin/ha-sim
ASM_DIR="asm"
OUTPUT_DIR="output"

$SIMULATOR -asm "$ASM_DIR/conv1d_simple.asm" 2> "$OUTPUT_DIR/sim_output.err"