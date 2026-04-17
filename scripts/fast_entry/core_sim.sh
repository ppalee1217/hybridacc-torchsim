#!/usr/bin/env bash
# core_sim.sh [build|clean|run|run-all|rebuild-fw]
#
# Fast-entry script for building, running, and testing the Core Controller
# simulation with real cluster SPM storage (test_core_sim).
#
# Unlike core_mcu_sim.sh (BusStub-based), this testbench instantiates
# FakeClusterSpm with actual byte-addressable storage and FakeDram,
# making it suitable for DMA and data-path verification tests.
#
# Examples:
#   core_sim.sh build
#   core_sim.sh run test_dma
#   core_sim.sh run test_dma --core-debug
#   core_sim.sh run-all
#   core_sim.sh clean

set -euo pipefail

MODE="${1:-help}"
shift 2>/dev/null || true

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL"
BUILD_DIR="$OUTPUT_DIR/core-sim-build"   # shared build dir (same CMake project)
CMAKEFILE_DIR="$SIM_MODEL_DIR/test"
FW_DIR="$SIM_MODEL_DIR/test/firmware"
LOG_DIR="$OUTPUT_DIR/core-sim-logs"

BINARY="$BUILD_DIR/test_core_sim"

# ── Default test list for run-all ──────────────────────────────────────────
# All firmware tests — core_sim uses real FakeClusterSpm + FakeDram storage.
# Add new entries here as more tests are created.
CORE_SIM_TESTS=(
    empty
    test_alu
    test_branch
    test_cluster_ctrl
    test_compound
    test_csr
    test_diag
    test_dma
    test_fabric
    test_hazard
    test_jump
    test_loadstore
    test_mul
    test_plic
    test_sram_timing
    test_stack
    test_trap
    test_wfi_timer
)

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
${BOLD}Core Simulation (with real cluster SPM) fast-entry${NC}

Usage:
  ${BOLD}core_sim.sh build${NC}            Build the simulation executable
  ${BOLD}core_sim.sh clean${NC}            Remove log directory
  ${BOLD}core_sim.sh run <test>${NC}       Run a single firmware test
  ${BOLD}core_sim.sh run-all${NC}          Run all core-sim tests
  ${BOLD}core_sim.sh rebuild-fw${NC}       Re-compile firmware ELFs for core-sim tests

Options (for run / run-all):
  --core-debug      Enable core pipeline debug trace
  --max-cycles N    Override max simulation cycles (default: 500000)

Core-sim tests (run-all):
EOF
    for name in "${CORE_SIM_TESTS[@]}"; do
        local elf="$FW_DIR/$name/${name}.elf"
        if [ -f "$elf" ]; then
            printf "  %-20s %s\n" "$name" "$elf"
        else
            printf "  %-20s ${YELLOW}(ELF not found)${NC}\n" "$name"
        fi
    done
}

# ── Build ──────────────────────────────────────────────────────────────────
do_build() {
    info "Building test_core_sim  →  $BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    pushd "$BUILD_DIR" > /dev/null
    cmake "$CMAKEFILE_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
    make test_core_sim -j"$(nproc)" 2>&1
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
    if [ -d "$LOG_DIR" ]; then
        rm -rf "$LOG_DIR"
        succ "Cleaned: $LOG_DIR"
    else
        info "Nothing to clean."
    fi
}

# ── Run single test ────────────────────────────────────────────────────────
# run_one <test_name> [extra_args...]
# Returns 0 if ALL TESTS PASSED, 1 otherwise.
run_one() {
    local name="$1"; shift
    local elf="$FW_DIR/$name/${name}.elf"
    local log="$LOG_DIR/${name}.log"

    if [ ! -f "$elf" ]; then
        err "ELF not found: $elf"
        return 1
    fi
    if [ ! -x "$BINARY" ]; then
        err "Executable not found — run 'core_sim.sh build' first"
        return 1
    fi

    local cmd=("$BINARY" "$elf")

    # Capture extra args
    local max_cycles=""
    local extra=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --max-cycles)   max_cycles="$2"; shift 2 ;;
            *)              extra+=("$1"); shift ;;
        esac
    done

    if [ -n "$max_cycles" ]; then
        cmd+=("$max_cycles")
    fi
    cmd+=("${extra[@]+"${extra[@]}"}")

    # Automatically enable DMA post-sim verification for test_dma
    if [[ "$name" == "test_dma" ]]; then
        cmd+=(--dma-check)
    fi

    info "Running ${BOLD}$name${NC}  →  $log"
    local rc=0
    "${cmd[@]}" > "$log" 2>&1 || rc=$?

    # Parse firmware test results from log
    # Format: "[TB] Total: N  Pass: N  Fail: N  First-fail: N"
    local fw_total=0 fw_pass=0 fw_fail=0 fw_ff=0
    local result_line
    result_line=$(grep -m1 '\[TB\] Total:' "$log" || true)
    if [ -n "$result_line" ]; then
        fw_total=$(echo "$result_line" | sed 's/.*Total: *\([0-9]*\).*/\1/')
        fw_pass=$(echo "$result_line" | sed 's/.*Pass: *\([0-9]*\).*/\1/')
        fw_fail=$(echo "$result_line" | sed 's/.*Fail: *\([0-9]*\).*/\1/')
        fw_ff=$(echo "$result_line" | sed 's/.*First-fail: *\([0-9]*\).*/\1/')
    fi

    # Check overall outcome
    if grep -q '\[TB\] ALL TESTS PASSED' "$log"; then
        succ "$name  ${GREEN}PASS${NC}  ($fw_pass/$fw_total assertions, rc=$rc)"
        return 0
    else
        err "$name  ${RED}FAIL${NC}  (fw=$fw_pass/$fw_total, fail=$fw_fail, first_fail_id=$fw_ff, rc=$rc)"
        # Show relevant failure lines
        grep '\[TB\] FAIL\|SOME TESTS FAILED\|Error\|ERROR' "$log" | head -10 | sed 's/^/    /'
        return 1
    fi
}

# ── Run all tests ──────────────────────────────────────────────────────────
do_run_all() {
    local total=0 pass=0 fail=0
    local failed_tests=()
    local extra_args=("$@")

    info "=== Run-all: core-sim tests (with real cluster SPM) ==="
    echo ""

    for name in "${CORE_SIM_TESTS[@]}"; do
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
    info "Rebuilding firmware ELFs for core-sim tests..."
    for name in "${CORE_SIM_TESTS[@]}"; do
        local d="$FW_DIR/$name"
        if [ ! -d "$d" ] || [ ! -f "$d/Makefile" ]; then
            warn "  $name — no Makefile, skipping"
            continue
        fi
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
            err "Missing test name.  Usage: core_sim.sh run <test>"
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
