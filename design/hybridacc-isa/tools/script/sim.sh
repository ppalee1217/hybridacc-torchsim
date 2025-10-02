ROOT_DIR=$(pwd)
BUILD_DIR=$ROOT_DIR/build
data_DIR=$ROOT_DIR/data/conv3x3
SIMULATOR=$ROOT_DIR/tools/bin/ha-sim
ASM_DIR="asm"
OUTPUT_DIR="output/sim"

if [ "$1" == "valgrind" ]; then
    VALGRIND_PREFIX="valgrind --tool=memcheck --track-origins=yes --leak-check=full"
else
    VALGRIND_PREFIX=""
fi

mkdir -p "$OUTPUT_DIR"

$VALGRIND_PREFIX \
    $SIMULATOR -asm "$ASM_DIR/conv1d_simple.asm" \
    --dump "$OUTPUT_DIR/conv1d_simple_dump.txt" \
    --data "$data_DIR" \
    --pol "$OUTPUT_DIR/conv1d_pol.bin" \
    -trace \
    2> "$OUTPUT_DIR/conv1d_simple.err"