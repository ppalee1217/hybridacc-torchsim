#!/usr/bin/env bash
# hybridacc_sim.sh [build|clean|test-fw|run-task]
#
# Fast-entry script for the HybridAcc SoC simulator (hybridacc-sim).
# Integrates CoreController + CmdToAhbBridge + real ComputeCluster + FakeDram.
#
# Examples:
#   hybridacc_sim.sh build
#   hybridacc_sim.sh test-fw                    Run all firmware tests
#   hybridacc_sim.sh test-fw test_alu           Run a single firmware test
#   hybridacc_sim.sh run-task test_dma --dma-check
#   hybridacc_sim.sh run-task test_alu --core-debug --max-cycles 100000
#   hybridacc_sim.sh clean

set -euo pipefail

MODE="${1:-help}"
shift 2>/dev/null || true

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL/simulator"
BUILD_DIR="$SIM_MODEL_DIR/build"
FW_DIR="$TOP_DIR/design/hybridacc-ESL/test/firmware"
LOG_DIR="$OUTPUT_DIR/hybridacc-sim-logs"

BINARY="$BUILD_DIR/bin/hybridacc-sim"

# ── Firmware test list ─────────────────────────────────────────────────────
# Tests that are compatible with the real ComputeCluster (not BusStub).
# test_fabric: 2 expected stub-only failures (T006/T008) — still included.
FW_TESTS=(
    empty
    test_alu
    test_branch
    test_compound
    test_csr
    test_dma
    test_fabric
    test_hazard
    test_jump
    test_loadstore
    test_mul
    test_plic
    test_stack
    test_trap
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
${BOLD}HybridAcc SoC Simulator (hybridacc-sim) fast-entry${NC}

Usage:
  ${BOLD}hybridacc_sim.sh build${NC}                  Build simulator (cmake + make, Release)
  ${BOLD}hybridacc_sim.sh build_debug${NC}            Build with DEBUG_UTILS enabled
  ${BOLD}hybridacc_sim.sh clean${NC}                  Remove build dir and logs
  ${BOLD}hybridacc_sim.sh test-fw${NC}                Run all firmware tests
  ${BOLD}hybridacc_sim.sh test-fw <name>${NC}         Run a single firmware test
  ${BOLD}hybridacc_sim.sh run-task <name|dir> [opts]${NC} Run firmware with custom options
  ${BOLD}hybridacc_sim.sh verify <dir>${NC}          Compare DRAM output with golden reference

  run-task accepts a firmware test name (e.g. test_alu) or a build output
  directory containing firmware.elf (e.g. output/hacc_conv3x3_test_build).
  If the directory also contains dram_init.bin, --mirror is added automatically.

Options (for test-fw / run-task):
  --core-debug      Enable core pipeline debug trace
  --dma-check       Enable DMA loopback verification (auto for test_dma)
  --fw-check        Enable DSRAM firmware test result verification
    --fast-boot       Preload core SRAM directly and bypass manifest loader
    --clock-period N  Override simulator clock period in ns (supports float, default: 2)
  --max-cycles N    Override max simulation cycles (default: 500000)
  --mirror FILE     Load/dump DRAM mirror image
  --trace FILE      Generate Perfetto trace JSON file
  --trace-level N   Trace detail level: 1=core 2=cluster 3=noc 4=pe (default: 2)
  -M SIZE           DRAM size (e.g. 1M, 256M)

Firmware tests:
EOF
    for name in "${FW_TESTS[@]}"; do
        local elf="$FW_DIR/$name/${name}.elf"
        if [ -f "$elf" ]; then
            printf "  %-20s %s\n" "$name" "${elf#"$TOP_DIR"/}"
        else
            printf "  %-20s ${YELLOW}(ELF not found)${NC}\n" "$name"
        fi
    done
}

# ── Build ──────────────────────────────────────────────────────────────────
do_build() {
    info "Building hybridacc-sim (Release)  →  $BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    pushd "$BUILD_DIR" > /dev/null
    cmake "$SIM_MODEL_DIR" -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_UTILS=OFF 2>&1 | tail -5
    make -j"$(nproc)" 2>&1
    popd > /dev/null
    if [ -x "$BINARY" ]; then
        succ "Build succeeded: $BINARY"
    else
        err "Build failed — executable not found"
        exit 1
    fi
}

# ── Build Debug ────────────────────────────────────────────────────────────
do_build_debug() {
    info "Building hybridacc-sim (Debug + DEBUG_UTILS)  →  $BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    pushd "$BUILD_DIR" > /dev/null
    cmake "$SIM_MODEL_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DENABLE_DEBUG_UTILS=ON \
        -DDEBUG_LEVEL_MIN=7 \
        2>&1 | tail -5
    make -j"$(nproc)" 2>&1
    popd > /dev/null
    if [ -x "$BINARY" ]; then
        succ "Build succeeded (debug): $BINARY"
    else
        err "Build failed — executable not found"
        exit 1
    fi
}

# ── Clean ──────────────────────────────────────────────────────────────────
do_clean() {
    local cleaned=0
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        succ "Cleaned build: $BUILD_DIR"
        cleaned=1
    fi
    if [ -d "$LOG_DIR" ]; then
        rm -rf "$LOG_DIR"
        succ "Cleaned logs:  $LOG_DIR"
        cleaned=1
    fi
    if [ $cleaned -eq 0 ]; then
        info "Nothing to clean."
    fi
}

# ── Run single task ────────────────────────────────────────────────────────
# run_task <name_or_dir> [extra simulator args...]
# Resolves the firmware ELF (and optional DRAM mirror) from either:
#   1. A build output directory containing firmware.elf  (e.g. output/hacc_conv3x3_test_build)
#   2. A firmware test name under $FW_DIR               (e.g. test_alu)
# Returns the simulator exit code. Extra args are forwarded directly, e.g.
#   --clock-period 1.5 --max-cycles 200000 --core-debug
run_task() {
    local name="$1"; shift
    local elf="" log="" task_dir=""

    # --- Resolve ELF and task directory ---
    # Try as a directory (absolute or relative to TOP_DIR) containing firmware.elf
    if [ -d "$name" ] && [ -f "$name/firmware.elf" ]; then
        task_dir=$(cd "$name" && pwd)
    elif [ -d "$TOP_DIR/$name" ] && [ -f "$TOP_DIR/$name/firmware.elf" ]; then
        task_dir=$(cd "$TOP_DIR/$name" && pwd)
    fi

    if [ -n "$task_dir" ]; then
        elf="$task_dir/firmware.elf"
        log="$task_dir/sim.log"
    else
        # Fall back to firmware test directory
        elf="$FW_DIR/$name/${name}.elf"
        log="$LOG_DIR/${name}.log"
    fi

    if [ ! -f "$elf" ]; then
        err "ELF not found: $elf"
        err "Provide a firmware test name or a build directory containing firmware.elf"
        return 1
    fi
    if [ ! -x "$BINARY" ]; then
        err "Executable not found — run 'hybridacc_sim.sh build' first"
        return 1
    fi

    local cmd=("$BINARY")

    # Auto-add --mirror if dram_init.bin exists in the task directory
    if [ -n "$task_dir" ] && [ -f "$task_dir/dram_init.bin" ]; then
        # Only add if user didn't already pass --mirror
        local has_mirror=0
        for a in "$@"; do [[ "$a" == "--mirror" ]] && has_mirror=1; done
        if [ $has_mirror -eq 0 ]; then
            cmd+=(--mirror "$task_dir/dram_init.bin")
            info "Auto-detected DRAM mirror: $task_dir/dram_init.bin"
        fi
    fi

    cmd+=("$@")             # pass through all extra args (--core-debug, --max-cycles, etc.)
    cmd+=("$elf")           # positional ELF must be last

    local display_name
    if [ -n "$task_dir" ]; then
        display_name="$(basename "$task_dir")"
    else
        display_name="$name"
    fi

    info "Running ${BOLD}${display_name}${NC}  →  $log"
    info "  cmd: ${cmd[*]}"

    local rc=0
    "${cmd[@]}" > "$log" 2>&1 || rc=$?

    # Show key output lines
    grep -E '\[(SIM|TB)\] (===|Total|ALL|SOME|DMA|EBREAK|Loader)' "$log" | sed 's/^/    /' || true

    return $rc
}

# ── Run single test (with pass/fail parsing) ───────────────────────────────
# run_test <test_name> [extra args...]
# Returns 0 if ALL TESTS PASSED, 1 otherwise.
run_test() {
    local name="$1"; shift
    local log="$LOG_DIR/${name}.log"

    # Build extra args; auto-add --dma-check for test_dma
    # auto-add --fw-check to parse DSRAM for tests
    local extra=("$@")
    local has_fw=0
    for a in "${extra[@]+"${extra[@]}"}"; do
        [[ "$a" == "--fw-check" ]] && has_fw=1
    done
    [ $has_fw -eq 0 ] && extra+=(--fw-check)

    if [[ "$name" == "test_dma" ]]; then
        local has_dma=0
        for a in "${extra[@]+"${extra[@]}"}"; do
            [[ "$a" == "--dma-check" ]] && has_dma=1
        done
        [ $has_dma -eq 0 ] && extra+=(--dma-check)
    fi

    run_task "$name" "${extra[@]+"${extra[@]}"}" || true

    # Parse firmware results from log
    local fw_total=0 fw_pass=0 fw_fail=0 fw_ff=0
    local result_line
    result_line=$(grep -m1 '\[SIM\] Total:' "$log" 2>/dev/null || true)
    if [ -n "$result_line" ]; then
        fw_total=$(echo "$result_line" | sed 's/.*Total: *\([0-9]*\).*/\1/')
        fw_pass=$(echo "$result_line"  | sed 's/.*Pass: *\([0-9]*\).*/\1/')
        fw_fail=$(echo "$result_line"  | sed 's/.*Fail: *\([0-9]*\).*/\1/')
        fw_ff=$(echo "$result_line"    | sed 's/.*First-fail: *\([0-9]*\).*/\1/')
    fi

    if grep -q 'ALL TESTS PASSED' "$log" 2>/dev/null; then
        succ "$name  ${GREEN}PASS${NC}  ($fw_pass/$fw_total assertions)"
        return 0
    else
        err "$name  ${RED}FAIL${NC}  (pass=$fw_pass/$fw_total, fail=$fw_fail, first_fail=$fw_ff)"
        grep -E 'FAIL|SOME TESTS|Error' "$log" 2>/dev/null | head -5 | sed 's/^/    /'
        return 1
    fi
}

# ── Test all firmware ──────────────────────────────────────────────────────
do_test_fw() {
    local target="${1:-}"
    shift 2>/dev/null || true
    local extra_args=("$@")

    # Single test mode
    if [ -n "$target" ]; then
        run_test "$target" "${extra_args[@]+"${extra_args[@]}"}"
        return $?
    fi

    # Run all
    local total=0 pass=0 fail=0
    local failed_tests=()

    info "=== HybridAcc SoC Simulator — firmware test suite ==="
    echo ""

    for name in "${FW_TESTS[@]}"; do
        total=$((total + 1))
        if run_test "$name" "${extra_args[@]+"${extra_args[@]}"}"; then
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
    echo "  Logs: $LOG_DIR/"

    return $fail
}

# ── Verify test output ──────────────────────────────────────────────────────
do_verify() {
    if [ $# -lt 1 ]; then
        err "Missing directory. Usage: hybridacc_sim.sh verify <dir>"
        return 1
    fi
    local dir="$1"
    if [ ! -f "$dir/dram_init.bin.out" ]; then
        err "dram_init.bin.out not found in $dir"
        return 1
    fi
    info "Running output verification for $dir"

    uv run python -m hybridacc_verify.check.compare_golden "$dir"
}

# ── Main dispatch ──────────────────────────────────────────────────────────
case "$MODE" in
    build)       do_build ;;
    build_debug) do_build_debug ;;
    clean)       do_clean ;;
    test-fw)  do_test_fw "$@" ;;
    verify)   do_verify "$@" ;;
    run-task)
        if [ $# -lt 1 ]; then
            err "Missing firmware name.  Usage: hybridacc_sim.sh run-task <name> [opts]"
            usage
            exit 1
        fi
        TASK_NAME="$1"; shift
        run_task "$TASK_NAME" "$@"
        ;;
    help|-h|--help) usage ;;
    *)
        err "Unknown mode: $MODE"
        usage
        exit 1
        ;;
esac
