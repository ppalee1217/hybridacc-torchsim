#!/usr/bin/env bash
set -euo pipefail

RISCV_TOOLCHAIN_VERSION="2024.09.03"
RISCV_TOOLCHAIN_TAG="2024.09.03"
PREFIX="${HOME}/.local/riscv"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix)
            PREFIX="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--prefix /path/to/install]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if [[ -f /etc/os-release ]]; then
    UBUNTU_VER="$(. /etc/os-release && echo "${VERSION_ID}")"
else
    UBUNTU_VER="22.04"
fi

case "${UBUNTU_VER}" in
    20.04) TARBALL_DISTRO="ubuntu-20.04" ;;
    *) TARBALL_DISTRO="ubuntu-22.04" ;;
esac

BASE_URL="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/${RISCV_TOOLCHAIN_TAG}"
TARBALL="riscv32-elf-${TARBALL_DISTRO}-gcc-nightly-${RISCV_TOOLCHAIN_VERSION}-nightly.tar.gz"

echo "[RISC-V] install prefix: ${PREFIX}"
mkdir -p "${PREFIX}"

TMPDIR="$(mktemp -d)"
trap 'rm -rf "${TMPDIR}"' EXIT

echo "[1/3] Download"
if command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "${TMPDIR}/${TARBALL}" "${BASE_URL}/${TARBALL}"
elif command -v curl >/dev/null 2>&1; then
    curl -fSL --progress-bar -o "${TMPDIR}/${TARBALL}" "${BASE_URL}/${TARBALL}"
else
    echo "Error: Neither wget nor curl is available."
    exit 1
fi

echo "[2/3] Extract"
tar -xzf "${TMPDIR}/${TARBALL}" -C "${PREFIX}" --strip-components=1

echo "[3/3] Verify"
GCC="${PREFIX}/bin/riscv32-unknown-elf-gcc"
if [[ -x "${GCC}" ]]; then
    "${GCC}" --version | head -1
else
    echo "Error: ${GCC} not found after extraction."
    exit 1
fi

echo "Done: RISC-V toolchain ready"
