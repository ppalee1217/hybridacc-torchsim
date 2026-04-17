#!/usr/bin/env bash
# run_e2e.sh — End-to-end pipeline: compile → gen-test-dram → simulate → verify
#
# Supports single or batch processing of workload YAML files.
#
# Usage:
#   run_e2e.sh <workload.yaml> [workload2.yaml ...] [options]
#   run_e2e.sh design/hybridacc-cc/example/*.yaml
#
# Options:
#   --output-dir DIR    Base output directory (default: output/)
#   --seed N            Random seed for test data (default: 42)
#   --max-cycles N      Max simulation cycles (default: 1000000)
#   --tolerance F       Cosine similarity threshold (default: 0.99)
#   --skip-build        Skip simulator build step
#   --skip-compile      Skip hacc-compile step (use existing build)
#   --skip-sim          Skip simulation step (verify existing output)
#   --core-debug        Enable core pipeline debug trace
#   --trace             Generate Perfetto trace file for each workload
#   --trace-level N     Trace level 1-4 (default: 2=cluster)
#   --verbose           Show detailed output
#   --dry-run           Print commands without executing

set -euo pipefail

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
DEFAULT_OUTPUT_DIR="$TOP_DIR/output"

# ── Defaults ───────────────────────────────────────────────────────────────
SEED=42
MAX_CYCLES=1000000
TOLERANCE=0.99
SKIP_BUILD=0
SKIP_COMPILE=0
SKIP_SIM=0
CORE_DEBUG=0
TRACE=0
TRACE_LEVEL=2
VERBOSE=0
DRY_RUN=0
OUTPUT_BASE=""

# ── Pretty print ──────────────────────────────────────────────────────────
RED=$(printf '\033[0;31m')
GREEN=$(printf '\033[0;32m')
YELLOW=$(printf '\033[0;33m')
BLUE=$(printf '\033[0;34m')
CYAN=$(printf '\033[0;36m')
BOLD=$(printf '\033[1m')
NC=$(printf '\033[0m')

info()  { printf "%s${BLUE}  [INFO]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }
succ()  { printf "%s${GREEN}  [ OK ]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }
warn()  { printf "%s${YELLOW}  [WARN]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }
err()   { printf "%s${RED}  [FAIL]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*" >&2; }
step()  { printf "%s${CYAN}  [STEP]${NC}  %s\n" "$(date '+%H:%M:%S')" "$*"; }

# ── Usage ──────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
${BOLD}HybridAcc End-to-End Pipeline${NC}

Usage:
  ${BOLD}run_e2e.sh${NC} <workload.yaml> [workload2.yaml ...] [options]

Pipeline: hacc-compile → gen-test-dram → hybridacc-sim → compare-golden

Options:
  --output-dir DIR    Base output directory (default: output/)
  --seed N            Random seed for test data generation (default: 42)
  --max-cycles N      Max simulation cycles (default: 1000000)
  --tolerance F       Cosine similarity threshold (default: 0.99)
  --skip-build        Skip simulator build step
  --skip-compile      Skip hacc-compile (reuse existing firmware)
  --skip-sim          Skip simulation (verify existing DRAM output)
  --core-debug        Enable core pipeline debug trace
  --trace             Generate Perfetto trace JSON for each workload
    --trace-level N     Trace detail level: 1=core 2=cluster 3=noc 4=pe (default: 2)
  --verbose           Show detailed output
  --dry-run           Print commands without executing

Examples:
  # Single workload
  run_e2e.sh design/hybridacc-cc/example/conv2d_3x3_example_test.yaml

  # Batch: all examples
  run_e2e.sh design/hybridacc-cc/example/*.yaml

  # Custom output and seed
  run_e2e.sh workload.yaml --output-dir output/mytest --seed 123
EOF
}

# ── Parse arguments ────────────────────────────────────────────────────────
WORKLOADS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   OUTPUT_BASE="$2"; shift 2 ;;
        --seed)         SEED="$2"; shift 2 ;;
        --max-cycles)   MAX_CYCLES="$2"; shift 2 ;;
        --tolerance)    TOLERANCE="$2"; shift 2 ;;
        --skip-build)   SKIP_BUILD=1; shift ;;
        --skip-compile) SKIP_COMPILE=1; shift ;;
        --skip-sim)     SKIP_SIM=1; shift ;;
        --core-debug)   CORE_DEBUG=1; shift ;;
        --trace)        TRACE=1; shift ;;
        --trace-level)  TRACE_LEVEL="$2"; shift 2 ;;
        --verbose)      VERBOSE=1; shift ;;
        --dry-run)      DRY_RUN=1; shift ;;
        -h|--help)      usage; exit 0 ;;
        -*)             err "Unknown option: $1"; usage; exit 1 ;;
        *)              WORKLOADS+=("$1"); shift ;;
    esac
done

if [[ ${#WORKLOADS[@]} -eq 0 ]]; then
    err "No workload YAML files specified."
    usage
    exit 1
fi

# ── Helper: run or print command ──────────────────────────────────────────
run_cmd() {
    if [[ $DRY_RUN -eq 1 ]]; then
        echo "  [DRY-RUN] $*"
        return 0
    fi
    if [[ $VERBOSE -eq 1 ]]; then
        "$@"
    else
        "$@" > /dev/null 2>&1
    fi
}

# ── Step 0: Ensure simulator is built ─────────────────────────────────────
if [[ $SKIP_BUILD -eq 0 && $SKIP_SIM -eq 0 ]]; then
    step "Building ESL simulator..."
    run_cmd "$SCRIPT_DIR/hybridacc_sim.sh" build
    succ "Simulator built."
fi

# ── Process each workload ─────────────────────────────────────────────────
TOTAL=${#WORKLOADS[@]}
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
RESULTS=()

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
info "Processing ${BOLD}$TOTAL${NC} workload(s) with seed=$SEED"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

for ((idx=0; idx<TOTAL; idx++)); do
    YAML="${WORKLOADS[$idx]}"
    BASENAME=$(basename "$YAML" .yaml)

    # Determine output directory
    if [[ -n "$OUTPUT_BASE" && $TOTAL -eq 1 ]]; then
        BUILD_DIR="$OUTPUT_BASE"
    elif [[ -n "$OUTPUT_BASE" ]]; then
        BUILD_DIR="$OUTPUT_BASE/${BASENAME}"
    else
        BUILD_DIR="$DEFAULT_OUTPUT_DIR/e2e_${BASENAME}"
    fi

    echo ""
    info "[$((idx+1))/$TOTAL] ${BOLD}$BASENAME${NC}"
    info "  YAML:   $YAML"
    info "  Output: $BUILD_DIR"

    # Resolve YAML path
    if [[ ! -f "$YAML" ]]; then
        # Try relative to TOP_DIR
        if [[ -f "$TOP_DIR/$YAML" ]]; then
            YAML="$TOP_DIR/$YAML"
        else
            err "  Workload not found: $YAML"
            FAIL_COUNT=$((FAIL_COUNT + 1))
            RESULTS+=("FAIL  $BASENAME  (workload not found)")
            continue
        fi
    fi

    FAILED=0

    # ── Step 1: Compile ───────────────────────────────────────────────
    if [[ $SKIP_COMPILE -eq 0 ]]; then
        step "  [1/4] Compiling firmware..."
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "  [DRY-RUN] hacc-compile $YAML -o $BUILD_DIR --dump-ir"
        else
            if uv run hacc-compile "$YAML" -o "$BUILD_DIR" --dump-ir 2>&1 | \
               { if [[ $VERBOSE -eq 1 ]]; then cat; else tail -3; fi; }; then
                succ "  Compilation done."
            else
                err "  Compilation failed!"
                FAILED=1
            fi
        fi
    else
        info "  [1/4] Skipping compilation (--skip-compile)"
    fi

    # ── Step 2: Generate test data ────────────────────────────────────
    if [[ $FAILED -eq 0 ]]; then
        step "  [2/4] Generating DRAM test data..."
        IR_PATH="$BUILD_DIR/hardware_ir.json"
        if [[ ! -f "$IR_PATH" && $DRY_RUN -eq 0 ]]; then
            err "  hardware_ir.json not found — did compilation succeed with --dump-ir?"
            FAILED=1
        else
            if [[ $DRY_RUN -eq 1 ]]; then
                echo "  [DRY-RUN] python -m hybridacc_verify.gen.gen_test_dram --ir $IR_PATH --workload $YAML --output-dir $BUILD_DIR --seed $SEED"
            else
                if uv run python -m hybridacc_verify.gen.gen_test_dram \
                    --ir "$IR_PATH" \
                    --workload "$YAML" \
                    --output-dir "$BUILD_DIR" \
                    --seed "$SEED" 2>&1 | \
                   { if [[ $VERBOSE -eq 1 ]]; then cat; else tail -5; fi; }; then
                    succ "  Test data generated."
                else
                    err "  Test data generation failed!"
                    FAILED=1
                fi
            fi
        fi
    fi

    # ── Step 3: Simulate ──────────────────────────────────────────────
    if [[ $FAILED -eq 0 && $SKIP_SIM -eq 0 ]]; then
        step "  [3/4] Running ESL simulation..."
        SIM_ARGS=("--max-cycles" "$MAX_CYCLES")
        if [[ $CORE_DEBUG -eq 1 ]]; then
            SIM_ARGS+=("--core-debug")
        fi
        if [[ $TRACE -eq 1 ]]; then
            SIM_ARGS+=("--trace" "$BUILD_DIR/trace.json" "--trace-level" "$TRACE_LEVEL")
        fi
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "  [DRY-RUN] hybridacc_sim.sh run-task $BUILD_DIR ${SIM_ARGS[*]}"
        else
            if "$SCRIPT_DIR/hybridacc_sim.sh" run-task "$BUILD_DIR" "${SIM_ARGS[@]}" 2>&1 | \
               { if [[ $VERBOSE -eq 1 ]]; then cat; else tail -5; fi; }; then
                succ "  Simulation done."
            else
                err "  Simulation failed or timed out!"
                FAILED=1
            fi
        fi
    elif [[ $SKIP_SIM -eq 1 ]]; then
        info "  [3/4] Skipping simulation (--skip-sim)"
    fi

    # ── Step 4: Verify ────────────────────────────────────────────────
    if [[ $FAILED -eq 0 ]]; then
        step "  [4/5] Verifying output..."
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "  [DRY-RUN] python -m hybridacc_verify.check.compare_golden $BUILD_DIR --tolerance $TOLERANCE"
        else
            VERIFY_OUTPUT=$(uv run python -m hybridacc_verify.check.compare_golden \
                "$BUILD_DIR" --tolerance "$TOLERANCE" 2>&1) || FAILED=1
            echo "$VERIFY_OUTPUT" | sed 's/^/    /'

            if [[ $FAILED -eq 0 ]]; then
                succ "  ${BOLD}$BASENAME${NC}: PASS"

                # ── Step 5: Trace analysis (optional) ──────────────────
                if [[ $TRACE -eq 1 && -f "$BUILD_DIR/trace.json" ]]; then
                    step "  [5/5] Analyzing trace..."
                    uv run python "$TOP_DIR/python/trace_parser/analyze_trace.py" \
                        "$BUILD_DIR/trace.json" \
                        --csv "$BUILD_DIR/trace_analysis.csv" 2>&1 | sed 's/^/    /'
                fi

                PASS_COUNT=$((PASS_COUNT + 1))
                RESULTS+=("PASS  $BASENAME")
            else
                err "  ${BOLD}$BASENAME${NC}: FAIL (verification)"
                FAIL_COUNT=$((FAIL_COUNT + 1))
                RESULTS+=("FAIL  $BASENAME  (verification)")
            fi
        fi
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("FAIL  $BASENAME  (pipeline error)")
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        SKIP_COUNT=$((SKIP_COUNT + 1))
        RESULTS+=("SKIP  $BASENAME  (dry-run)")
    fi
done

# ── Summary ────────────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "${BOLD}Summary${NC}  (seed=$SEED, tolerance=$TOLERANCE)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
for r in "${RESULTS[@]}"; do
    if [[ "$r" == PASS* ]]; then
        echo "  ${GREEN}$r${NC}"
    elif [[ "$r" == FAIL* ]]; then
        echo "  ${RED}$r${NC}"
    else
        echo "  ${YELLOW}$r${NC}"
    fi
done
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [[ $DRY_RUN -eq 1 ]]; then
    info "Dry-run complete. No commands were executed."
elif [[ $FAIL_COUNT -eq 0 ]]; then
    succ "${BOLD}All $PASS_COUNT test(s) PASSED.${NC}"
else
    err "${BOLD}$FAIL_COUNT / $TOTAL test(s) FAILED.${NC} ($PASS_COUNT passed)"
fi

exit $FAIL_COUNT
