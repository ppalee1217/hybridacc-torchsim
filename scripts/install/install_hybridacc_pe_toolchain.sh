#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PE_BIN_DIR="${PROJECT_ROOT}/design/hybridacc-pe-isa/tools/bin"
LINK_DIR="${HOME}/.local/bin"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --link-dir)
            LINK_DIR="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--link-dir /path/to/bin]"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

if [[ ! -d "${PE_BIN_DIR}" ]]; then
    echo "Error: PE tool directory not found: ${PE_BIN_DIR}"
    exit 1
fi

mkdir -p "${LINK_DIR}"

tools=(ha-asm ha-objdump ha-package ha-sim)
for tool in "${tools[@]}"; do
    src="${PE_BIN_DIR}/${tool}"
    if [[ ! -f "${src}" ]]; then
        echo "Warning: ${src} missing, skip"
        continue
    fi
    chmod +x "${src}"
    ln -sfn "${src}" "${LINK_DIR}/${tool}"
    echo "Linked ${tool} -> ${LINK_DIR}/${tool}"
done

echo "Done: HybridAcc PE ISA tools ready"
