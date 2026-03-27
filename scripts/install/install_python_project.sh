#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if ! command -v uv >/dev/null 2>&1; then
    echo "uv not found. Installing uv..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    if [ -d "${HOME}/.local/bin" ]; then
        export PATH="${HOME}/.local/bin:${PATH}"
    fi
fi

cd "${PROJECT_ROOT}"
uv sync

echo "Done: Python project dependencies installed"
