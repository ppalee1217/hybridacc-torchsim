#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
TEST_DIR="$TOP_DIR/design/hybridacc-ESL/test"
BUILD_DIR="$TEST_DIR/build"
SYSTEMC_ROOT="${SYSTEMC_LIB:-$TOP_DIR/libs/systemc-2.3.3}"

MODE="${1:-help}"
TARGET="${2:-all}"

if [[ "$MODE" != "build" && "$MODE" != "run" && "$MODE" != "all" && "$MODE" != "clean" && "$MODE" != "help" && "$MODE" != "-h" && "$MODE" != "--help" ]]; then
    TARGET="$MODE"
    MODE="run"
fi

usage() {
    cat <<EOF
Usage:
  ./scripts/fast_entry/cluster_test.sh build [agu|hddu|spm|noc|cluster|all]
  ./scripts/fast_entry/cluster_test.sh run [agu|hddu|spm|noc|cluster|all]
  ./scripts/fast_entry/cluster_test.sh all
  ./scripts/fast_entry/cluster_test.sh clean

Short form:
  ./scripts/fast_entry/cluster_test.sh agu
  ./scripts/fast_entry/cluster_test.sh hddu
  ./scripts/fast_entry/cluster_test.sh spm
  ./scripts/fast_entry/cluster_test.sh noc
  ./scripts/fast_entry/cluster_test.sh cluster
EOF
}

resolve_targets() {
    case "$1" in
        agu) echo "test_agu_unit" ;;
        hddu) echo "test_hddu_unit" ;;
        spm) echo "test_spm_unit" ;;
        noc) echo "test_noc_unit" ;;
        cluster) echo "test_cluster_unit" ;;
        all) echo "test_agu_unit test_hddu_unit test_spm_unit test_noc_unit test_cluster_unit" ;;
        *)
            echo "Unknown target: $1" >&2
            exit 1
            ;;
    esac
}

ensure_configured() {
    mkdir -p "$BUILD_DIR"
    SYSTEMC_LIB="$SYSTEMC_ROOT" cmake -S "$TEST_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
}

build_targets() {
    ensure_configured
    cmake --build "$BUILD_DIR" --target "$@"
}

run_target() {
    local bin="$1"
    "$BUILD_DIR/$bin"
}

case "$MODE" in
    build)
        read -r -a targets <<< "$(resolve_targets "$TARGET")"
        build_targets "${targets[@]}"
        ;;
    run)
        read -r -a targets <<< "$(resolve_targets "$TARGET")"
        build_targets "${targets[@]}"
        for target in "${targets[@]}"; do
            run_target "$target"
        done
        ;;
    all)
        read -r -a targets <<< "$(resolve_targets all)"
        build_targets "${targets[@]}"
        for target in "${targets[@]}"; do
            run_target "$target"
        done
        ;;
    clean)
        rm -rf "$BUILD_DIR"
        ;;
    help|-h|--help)
        usage
        ;;
esac
