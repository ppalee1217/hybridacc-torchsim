# run.sh [run|build|clean]

MODE=$1

if [ "$MODE" == "run" ]; then
    ./build/test_noc_sim ~/work/MasterResearch/HybridAcc/output/noc/conv2d > ./build/out.log
elif [ "$MODE" == "build" ]; then
    if [ -d "build" ]; then
        rm -rf build
    fi
    mkdir -p build
    cd build
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_PE_COMPONENTS ..
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_NOC_TOP ..
    cmake -DENABLE_DEBUG_UTILS=OFF ..
    make test_noc_sim -j8
    cd ..
elif [ "$MODE" == "clean" ]; then
    if [ -d "build" ]; then
        rm -rf build
    fi
else
    echo "Usage: run.sh [run|build|clean]"
fi