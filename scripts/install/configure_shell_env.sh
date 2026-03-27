#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BASHRC="${HOME}/.bashrc"
RISCV_PREFIX="${HOME}/.local/riscv"
SHOULD_SOURCE=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --riscv-prefix)
            RISCV_PREFIX="$2"
            shift 2
            ;;
        --no-source)
            SHOULD_SOURCE=0
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [--riscv-prefix /path] [--no-source]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

MARK_BEGIN="# >>> hybridacc env >>>"
MARK_END="# <<< hybridacc env <<<"

mkdir -p "$(dirname "${BASHRC}")"
touch "${BASHRC}"

ENV_BLOCK="${MARK_BEGIN}
export HYBRIDACC_ROOT=\"${PROJECT_ROOT}\"
export SYSTEMC_LIB=\"${PROJECT_ROOT}/libs/systemc-2.3.3\"
export RISCV_TOOLCHAIN_ROOT=\"${RISCV_PREFIX}\"
export PATH=\"${PROJECT_ROOT}/design/hybridacc-pe-isa/tools/bin:${RISCV_PREFIX}/bin:${HOME}/.local/bin:\$PATH\"
${MARK_END}"

if grep -q "${MARK_BEGIN}" "${BASHRC}"; then
    awk -v begin="${MARK_BEGIN}" -v end="${MARK_END}" -v block="${ENV_BLOCK}" '
    $0 == begin {
        print block
        in_block = 1
        next
    }
    $0 == end {
        in_block = 0
        next
    }
    !in_block { print }
    ' "${BASHRC}" > "${BASHRC}.tmp"
    mv "${BASHRC}.tmp" "${BASHRC}"
    echo "Updated existing HybridAcc env block in ${BASHRC}"
else
    printf "\n%s\n" "${ENV_BLOCK}" >> "${BASHRC}"
    echo "Inserted HybridAcc env block into ${BASHRC}"
fi

if [[ "${SHOULD_SOURCE}" == "1" ]]; then
    # shellcheck disable=SC1090
    source "${BASHRC}"
    echo "Sourced ${BASHRC} in current shell"
fi

echo "Done: shell environment configured"
