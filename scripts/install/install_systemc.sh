#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PACKAGES_DIR="${PROJECT_ROOT}/packages"
LIBS_DIR="${PROJECT_ROOT}/libs"
SYSTEMC_VERSION="2.3.3"

SYSTEMC_PACKAGE="${PACKAGES_DIR}/systemc-${SYSTEMC_VERSION}"
SYSTEMC_LIB="${LIBS_DIR}/systemc-${SYSTEMC_VERSION}"

echo "[SystemC] project root: ${PROJECT_ROOT}"

echo "[1/2] Ensure source package"
if [ -d "${SYSTEMC_PACKAGE}" ]; then
    echo "  Found ${SYSTEMC_PACKAGE}"
else
    mkdir -p "${PACKAGES_DIR}"
    cd "${PACKAGES_DIR}"
    if command -v wget >/dev/null 2>&1; then
        wget -c "https://www.accellera.org/images/downloads/standards/systemc/systemc-${SYSTEMC_VERSION}.tar.gz" -O systemc.tar.gz
    elif command -v curl >/dev/null 2>&1; then
        curl -L "https://www.accellera.org/images/downloads/standards/systemc/systemc-${SYSTEMC_VERSION}.tar.gz" -o systemc.tar.gz
    else
        echo "Error: Neither wget nor curl is available."
        exit 1
    fi
    tar -xzf systemc.tar.gz
    rm -f systemc.tar.gz
fi

echo "[2/2] Build and install"
if [ -d "${SYSTEMC_LIB}" ]; then
    echo "  Already installed at ${SYSTEMC_LIB}"
else
    mkdir -p "${SYSTEMC_PACKAGE}/build"
    cd "${SYSTEMC_PACKAGE}/build"
    cmake "${SYSTEMC_PACKAGE}" -DCMAKE_INSTALL_PREFIX="${SYSTEMC_LIB}" -DCMAKE_CXX_STANDARD=17
    make -j"$(nproc)"
    make install
    rm -rf "${SYSTEMC_PACKAGE}/build"
    echo "  Installed to ${SYSTEMC_LIB}"
fi

echo "Done: SystemC ready"
