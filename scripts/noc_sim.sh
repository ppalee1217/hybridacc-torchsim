# noc_sim.sh [run|build|clean]

MODE=$1
TB_NAME="conv_k3c4h8"  # Default testbench name

# if $2 is provided, use it as TB_NAME
if [ ! -z "$2" ]; then
    TB_NAME=$2
fi

TOP_DIR=$(pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL"
BUILD_DIR="$SIM_MODEL_DIR/build/noc_sim"
SIM_DATA_DIR="$OUTPUT_DIR/noc-sim/$TB_NAME" # Default simulation data directory
OUTPUT_LOG="$OUTPUT_DIR/out-$TB_NAME.log"
CMAKEFILE_DIR="$SIM_MODEL_DIR/test"

if [ "$MODE" == "run" ]; then
    "$BUILD_DIR"/test_noc_sim -c 1 -t ./output/trace-$TB_NAME.json $SIM_DATA_DIR > $OUTPUT_LOG
    echo "Simulation completed. Output log saved to $OUTPUT_LOG"
elif [ "$MODE" == "build" ]; then
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_PE_COMPONENTS $CMAKEFILE_DIR
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_PE_TOP $CMAKEFILE_DIR
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_NOC_COMPONENTS $CMAKEFILE_DIR
    cmake -DENABLE_DEBUG_UTILS=OFF $CMAKEFILE_DIR
    make test_noc_sim -j8
    cd ..
elif [ "$MODE" == "clean" ]; then
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
else
    echo "Usage: noc_sim.sh [run|build|clean]"
fi