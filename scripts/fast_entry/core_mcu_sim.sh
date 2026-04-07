#!/usr/bin/env bash
# core_mcu_sim.sh [build|clean|run|run-all|rebuild-fw]
#
# Fast-entry script for building, running, and testing the Core MCU
# simulation (ELF-based RISC-V firmware tests).
#
# Examples:
#   core_mcu_sim.sh build
#   core_mcu_sim.sh run test_alu
#   core_mcu_sim.sh run test_alu --core-debug
#   core_mcu_sim.sh run-all
#   core_mcu_sim.sh run-all --dump-results
#   core_mcu_sim.sh clean

set -euo pipefail

MODE="${1:-help}"
shift 2>/dev/null || true

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL"
BUILD_DIR="$OUTPUT_DIR/core-mcu-sim-build"
CMAKEFILE_DIR="$SIM_MODEL_DIR/test"
FW_DIR="$SIM_MODEL_DIR/test/firmware"
LOG_DIR="$OUTPUT_DIR/core-mcu-logs"

BINARY="$BUILD_DIR/test_core_mcu_sim"

# ── Pretty print ──────────────────────────────────────────────────────────
RED=$(printf '\033[0;31m')
GREEN=$(printf '\033[0;32m')
YELLOW=$(printf '\033[0;33m')
BLUE=$(printf '\033[0;34m')
BOLD=$(printf '\033[1m')
NC=$(printf '\033[0m')

info() { printf "%s${BLUE} [INFO]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }
succ() { printf "%s${GREEN} [ OK ]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }
warn() { printf "%s${YELLOW} [WARN]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }
err()  { printf "%s${RED} [FAIL]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*" >&2; }

mkdir -p "$OUTPUT_DIR" "$LOG_DIR"

# ── Usage ──────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}Core MCU Simulation fast-entry${NC}

Usage:
  ${BOLD}core_mcu_sim.sh build${NC}            Build the simulation executable
  ${BOLD}core_mcu_sim.sh clean${NC}            Remove build directory
  ${BOLD}core_mcu_sim.sh run <test>${NC}       Run a single firmware test
  ${BOLD}core_mcu_sim.sh run-all${NC}          Run all firmware tests
  ${BOLD}core_mcu_sim.sh rebuild-fw${NC}       Re-compile all firmware ELFs

Options (for run / run-all):
  --core-debug      Enable core pipeline debug trace
  --dump-results    Dump DSRAM test results (first 64 bytes)
  --max-cycles N    Override max simulation cycles (default: 500000)

Available tests:
EOF
    for d in "$FW_DIR"/*/; do
        [ -d "$d" ] || continue
        local name
        name=$(basename "$d")
        if [ -f "$d/${name}.elf" ]; then
            printf "  %-20s %s\n" "$name" "$d${name}.elf"
        fi
    done
}

# ── Build ──────────────────────────────────────────────────────────────────
do_build() {
    info "Building test_core_mcu_sim  →  $BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    pushd "$BUILD_DIR" > /dev/null
    cmake "$CMAKEFILE_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
    make test_core_mcu_sim -j"$(nproc)" 2>&1
    popd > /dev/null
    if [ -x "$BINARY" ]; then
        succ "Build succeeded: $BINARY"
    else
        err "Build failed — executable not found"
        exit 1
    fi
}

# ── Clean ──────────────────────────────────────────────────────────────────
do_clean() {
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        succ "Cleaned: $BUILD_DIR"
    else
        info "Nothing to clean."
    fi
}

# ── Run single test ────────────────────────────────────────────────────────
# run_one <test_name> [extra_args...]
# Returns 0 if TB PASS and firmware PASS, 1 otherwise.
run_one() {
    local name="$1"; shift
    local elf="$FW_DIR/$name/${name}.elf"
    local log="$LOG_DIR/${name}.log"

    if [ ! -f "$elf" ]; then
        err "ELF not found: $elf"
        return 1
    fi
    if [ ! -x "$BINARY" ]; then
        err "Executable not found — run 'core_mcu_sim.sh build' first"
        return 1
    fi

    local cmd=("$BINARY" "$elf")

    # Capture extra args
    local max_cycles=""
    local dump_results=0
    local extra=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --max-cycles)   max_cycles="$2"; shift 2 ;;
            --dump-results) dump_results=1; shift ;;
            *)              extra+=("$1"); shift ;;
        esac
    done

    if [ -n "$max_cycles" ]; then
        cmd+=("$max_cycles")
    fi
    cmd+=("${extra[@]+"${extra[@]}"}")

    # Always dump DSRAM result area (first 16 words = 64 bytes)
    cmd+=(--dump-dsram 0x0 0x40)

    info "Running ${BOLD}$name${NC}  →  $log"
    if ! "${cmd[@]}" > "$log" 2>&1; then
        err "$name — simulator returned non-zero"
        tail -5 "$log" | sed 's/^/    /'
        return 1
    fi

    # Parse TB-level outcome
    local tb_pass=0
    if grep -q '\[TB\] PASS' "$log"; then
        tb_pass=1
    fi

    # Parse firmware-level results from DSRAM dump
    # Word layout: [0]=total [1]=pass [2]=fail [3]=first_fail_id
    local fw_total=0 fw_pass=0 fw_fail=0 fw_ff=0
    local dsram_line
    dsram_line=$(grep -m1 '0x10000000:' "$log" || true)
    if [ -n "$dsram_line" ]; then
        # Line format: "  0x10000000: AAAAAAAA BBBBBBBB CCCCCCCC DDDDDDDD  |....|"
        local words
        words=$(echo "$dsram_line" | sed 's/.*: //;s/ *|.*//')
        fw_total=$((16#$(echo "$words" | awk '{print $1}')))
        fw_pass=$((16#$(echo "$words" | awk '{print $2}')))
        fw_fail=$((16#$(echo "$words" | awk '{print $3}')))
        fw_ff=$((16#$(echo "$words" | awk '{print $4}')))
    fi

    # Treat uninitialized DSRAM (e.g. 0xCAFECAFE fill) as no firmware assertions
    if [ "$fw_total" -gt 100000 ]; then
        fw_total=0; fw_pass=0; fw_fail=0; fw_ff=0
    fi

    # Emit result line
    if [ $tb_pass -eq 1 ] && [ $fw_fail -eq 0 ] && [ $fw_total -gt 0 ]; then
        succ "$name  ${GREEN}PASS${NC}  ($fw_pass/$fw_total assertions)"
        return 0
    elif [ $tb_pass -eq 1 ] && [ $fw_total -eq 0 ]; then
        # e.g. "empty" test — no assertions, just EBREAK
        succ "$name  ${GREEN}PASS${NC}  (EBREAK, 0 assertions)"
        return 0
    else
        err "$name  ${RED}FAIL${NC}  (TB=$tb_pass, fw=$fw_pass/$fw_total, fail=$fw_fail, first_fail_id=$fw_ff)"
        if [ $dump_results -eq 1 ] || [ $fw_fail -gt 0 ]; then
            grep 'DSRAM\|0x1000' "$log" | head -20 | sed 's/^/    /'
        fi
        return 1
    fi
}

# ── Run all tests ──────────────────────────────────────────────────────────
do_run_all() {
    local total=0 pass=0 fail=0
    local failed_tests=()
    local extra_args=("$@")

    info "=== Run-all: firmware tests ==="
    echo ""

    for d in "$FW_DIR"/*/; do
        [ -d "$d" ] || continue
        local name
        name=$(basename "$d")
        [ -f "$d/${name}.elf" ] || continue

        total=$((total + 1))
        if run_one "$name" "${extra_args[@]+"${extra_args[@]}"}"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            failed_tests+=("$name")
        fi
    done

    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    if [ $fail -eq 0 ]; then
        succ "All $total tests passed."
    else
        err "$fail / $total tests failed: ${failed_tests[*]}"
    fi
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    return $fail
}

# ── Rebuild firmware ───────────────────────────────────────────────────────
do_rebuild_fw() {
    info "Rebuilding firmware ELFs..."
    for d in "$FW_DIR"/*/; do
        [ -d "$d" ] || continue
        [ -f "$d/Makefile" ] || continue
        local name
        name=$(basename "$d")
        info "  make -C $d"
        make -C "$d" clean all 2>&1 | sed 's/^/    /'
        if [ -f "$d/${name}.elf" ]; then
            succ "  $name.elf"
        else
            err "  $name.elf — build failed"
        fi
    done
}

# ── Main dispatch ──────────────────────────────────────────────────────────
case "$MODE" in
    build)    do_build ;;
    clean)    do_clean ;;
    run)
        if [ $# -lt 1 ]; then
            err "Missing test name.  Usage: core_mcu_sim.sh run <test>"
            usage
            exit 1
        fi
        TEST_NAME="$1"; shift
        run_one "$TEST_NAME" "$@"
        ;;
    run-all)  do_run_all "$@" ;;
    rebuild-fw) do_rebuild_fw ;;
    help|-h|--help) usage ;;
    *)
        err "Unknown mode: $MODE"
        usage
        exit 1
        ;;
esac