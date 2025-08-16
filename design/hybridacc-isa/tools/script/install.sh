ROOT_DIR=$(pwd)
BUILD_DIR=$ROOT_DIR/build
BINARY_DIR=$ROOT_DIR/tools

mkdir -p $BUILD_DIR
cd $BUILD_DIR
cmake .. -DBUILD_TESTS=ON
cmake --build . -j$(nproc) && cmake --install . --prefix $BINARY_DIR