#!/bin/bash
VERSION=${1:-"2.3.3"}

if [ -d "${LIBRARY_DIR}/systemc" ]; then
    echo "SystemC is already installed in ${LIBRARY_DIR}/systemc"
    exit 0
else
    echo "Installing systemc version ${VERSION} ..."

    mkdir -p $PACKAGE_DIR
    cd $PACKAGE_DIR

    # download and install systemc
    wget https://accellera.org/images/downloads/standards/systemc/systemc-${VERSION}.tar.gz
    tar -xvf systemc-${VERSION}.tar.gz
    cd systemc-${VERSION}
    mkdir build && cd build
    ../configure --prefix=${LIBRARY_DIR}/systemc
    make -j$(nproc)
    make install

    # cleanup
    cd $PACKAGE_DIR
    rm -rf systemc-${VERSION}
    rm systemc-${VERSION}.tar.gz
    echo "SystemC version ${VERSION} installed successfully. -> ${LIBRARY_DIR}/systemc"
fi

# export environment variables
export SYSTEMC_HOME=${LIBRARY_DIR}/systemc
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:${SYSTEMC_HOME}/lib-linux64
export CPLUS_INCLUDE_PATH=$CPLUS_INCLUDE_PATH:${SYSTEMC_HOME}/include