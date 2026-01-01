# noc_sim.sh [run|build|clean]

MODE=$1

TOP_DIR=$(pwd)
BUILD_DIR="$TOP_DIR/build/noc_sim"
SIM_DATA_DIR="$TOP_DIR/../../output/noc/conv2d" # Default simulation data directory
OUTPUT_LOG="$BUILD_DIR/out.log"
CMAKEFILE_DIR="$TOP_DIR/test"

if [ "$MODE" == "run" ]; then
    "$BUILD_DIR"/test_noc_sim $SIM_DATA_DIR > $OUTPUT_LOG
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