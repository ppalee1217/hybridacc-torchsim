ROOT_DIR=$(pwd)
BUILD_DIR=$ROOT_DIR/build

cd $BUILD_DIR
ctest -C $BUILD_DIR --output-on-failure