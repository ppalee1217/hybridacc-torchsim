#!/usr/bin/env bash
# run_e2e.sh — End-to-end pipeline: compile → gen-test-dram → simulate → verify
#
# Supports single or batch processing of workload YAML files.
# Batch mode runs up to `nproc` workloads in parallel by default and shows a
# live progress dashboard in the terminal.

set -euo pipefail

# ── Paths ──────────────────────────────────────────────────────────────────
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
DEFAULT_OUTPUT_DIR="$TOP_DIR/output"

# ── Defaults ───────────────────────────────────────────────────────────────
SEED=42
MAX_CYCLES=100000000
CLOCK_PERIOD_NS="2"
TOLERANCE=0.99
SKIP_BUILD=0
SKIP_COMPILE=0
SKIP_SIM=0
RESUME=0
CORE_DEBUG=0
FAST_BOOT=0
TRACE=0
TRACE_LEVEL=2
VERBOSE=0
DRY_RUN=0
OUTPUT_BASE=""
JOBS=0

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
  --clock-period F    Simulator clock period in ns (default: 2)
  --tolerance F       Cosine similarity threshold (default: 0.99)
  --jobs N            Parallel workload jobs (default: nproc in batch mode)
  --skip-build        Skip simulator build step
  --skip-compile      Skip hacc-compile (reuse existing firmware)
  --skip-sim          Skip simulation (verify existing DRAM output)
    --resume            Skip workloads with an existing final e2e result marker
  --core-debug        Enable core pipeline debug trace
  --fast-boot         Preload core SRAM directly and bypass manifest loader
  --trace             Generate Perfetto trace JSON for each workload
  --trace-level N     Trace detail level: 1=core 2=cluster 3=noc 4=pe (default: 2)
  --verbose           Show detailed output in sequential mode
  --dry-run           Print commands without executing

Examples:
  # Single workload
  run_e2e.sh design/hybridacc-cc/example/conv2d_3x3_example_test.yaml

  # Batch: all examples, auto-parallelized with nproc jobs
  run_e2e.sh design/hybridacc-cc/example/*.yaml

  # Custom output, seed, and worker count
  run_e2e.sh workload.yaml workload2.yaml --output-dir output/mytest --seed 123 --jobs 8
EOF
}

# ── Parse arguments ────────────────────────────────────────────────────────
WORKLOADS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir)   OUTPUT_BASE="$2"; shift 2 ;;
        --seed)         SEED="$2"; shift 2 ;;
        --max-cycles)   MAX_CYCLES="$2"; shift 2 ;;
        --clock-period) CLOCK_PERIOD_NS="$2"; shift 2 ;;
        --tolerance)    TOLERANCE="$2"; shift 2 ;;
        --jobs)         JOBS="$2"; shift 2 ;;
        --skip-build)   SKIP_BUILD=1; shift ;;
        --skip-compile) SKIP_COMPILE=1; shift ;;
        --skip-sim)     SKIP_SIM=1; shift ;;
        --resume)       RESUME=1; shift ;;
        --core-debug)   CORE_DEBUG=1; shift ;;
        --fast-boot)    FAST_BOOT=1; shift ;;
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

default_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
    else
        echo 1
    fi
}

resolve_yaml_path() {
    local yaml="$1"
    if [[ -f "$yaml" ]]; then
        printf '%s\n' "$yaml"
        return 0
    fi
    if [[ -f "$TOP_DIR/$yaml" ]]; then
        printf '%s\n' "$TOP_DIR/$yaml"
        return 0
    fi
    return 1
}

build_dir_for_workload() {
    local yaml="$1"
    local total="$2"
    local basename
    basename=$(basename "$yaml" .yaml)
    if [[ -n "$OUTPUT_BASE" && $total -eq 1 ]]; then
        printf '%s\n' "$OUTPUT_BASE"
    elif [[ -n "$OUTPUT_BASE" ]]; then
        printf '%s/%s\n' "$OUTPUT_BASE" "$basename"
    else
        printf '%s/e2e_%s\n' "$DEFAULT_OUTPUT_DIR" "$basename"
    fi
}

resume_result_file_for_build_dir() {
    local build_dir="$1"
    printf '%s/.e2e.result\n' "$build_dir"
}

select_build_dir_for_workload() {
    local yaml="$1"
    local total="$2"
    local basename
    local primary
    local alternate=""
    local primary_result
    local alternate_result

    basename=$(basename "$yaml" .yaml)
    primary=$(build_dir_for_workload "$yaml" "$total")
    RESOLVED_BUILD_DIR="$primary"

    if [[ $RESUME -eq 0 ]]; then
        return 0
    fi

    if [[ -f "$(resume_result_file_for_build_dir "$primary")" ]]; then
        return 0
    fi

    if [[ -n "$OUTPUT_BASE" && $total -eq 1 ]]; then
        alternate="$OUTPUT_BASE/$basename"
        primary_result=$(resume_result_file_for_build_dir "$primary")
        alternate_result=$(resume_result_file_for_build_dir "$alternate")
        if [[ -f "$alternate_result" ]]; then
            RESOLVED_BUILD_DIR="$alternate"
        elif [[ ! -f "$primary_result" && ( -f "$alternate/e2e_run.log" || -f "$alternate/hardware_ir.json" || -f "$alternate/trace.json" ) ]]; then
            RESOLVED_BUILD_DIR="$alternate"
        fi
    fi
}

read_persistent_result_for_build_dir() {
    local build_dir="$1"
    local result_file

    result_file=$(resume_result_file_for_build_dir "$build_dir")
    [[ -f "$result_file" ]] || return 1
    read_result_file "$result_file"
}

write_persistent_result_for_build_dir() {
    local build_dir="$1"
    local result="$2"
    local name="$3"
    local detail="$4"
    local log_path="$5"

    mkdir -p "$build_dir"
    write_result_file "$(resume_result_file_for_build_dir "$build_dir")" "$result" "$name" "$detail" "$log_path"
}

prepare_workloads_for_execution() {
    local total="$1"
    local idx
    local yaml_input
    local basename
    local build_dir
    local detail
    local active_workloads=()
    local active_build_dirs=()

    for ((idx = 0; idx < total; idx++)); do
        yaml_input="${WORKLOADS[$idx]}"
        basename=$(basename "$yaml_input" .yaml)
        select_build_dir_for_workload "$yaml_input" "$total"
        build_dir="$RESOLVED_BUILD_DIR"

        if [[ $RESUME -eq 1 ]] && read_persistent_result_for_build_dir "$build_dir"; then
            RESUME_SKIP_COUNT=$((RESUME_SKIP_COUNT + 1))
            SKIP_COUNT=$((SKIP_COUNT + 1))

            if [[ "$RESULT_VALUE" == "PASS" ]]; then
                PASS_COUNT=$((PASS_COUNT + 1))
                RESULTS+=("PASS  $basename  (resume)")
            else
                FAIL_COUNT=$((FAIL_COUNT + 1))
                detail="$RESULT_DETAIL"
                if [[ -n "$RESULT_LOG_PATH" ]]; then
                    if [[ -n "$detail" ]]; then
                        detail="$detail; log=$RESULT_LOG_PATH"
                    else
                        detail="log=$RESULT_LOG_PATH"
                    fi
                fi
                if [[ -z "$detail" ]]; then
                    detail="resume"
                fi
                RESULTS+=("FAIL  $basename  ($detail)")
            fi
            continue
        fi

        active_workloads+=("$yaml_input")
        active_build_dirs+=("$build_dir")
    done

    WORKLOADS=("${active_workloads[@]}")
    WORKLOAD_BUILD_DIRS=("${active_build_dirs[@]}")
}

phase_index() {
    case "$1" in
        queued) echo 0 ;;
        compile) echo 1 ;;
        gendram) echo 2 ;;
        sim) echo 3 ;;
        verify) echo 4 ;;
        trace) echo 5 ;;
        done|failed|skipped) echo 6 ;;
        *) echo 0 ;;
    esac
}

write_status_file() {
    local file="$1"
    local phase="$2"
    local name="$3"
    local message="$4"
    printf '%s\037%s\037%s\n' "$phase" "$name" "$message" > "$file"
}

write_result_file() {
    local file="$1"
    local result="$2"
    local name="$3"
    local detail="$4"
    local log_path="$5"
    printf '%s\037%s\037%s\037%s\n' "$result" "$name" "$detail" "$log_path" > "$file"
}

read_status_file() {
    local file="$1"
    IFS=$'\037' read -r STATUS_PHASE STATUS_NAME STATUS_MESSAGE < "$file"
}

read_result_file() {
    local file="$1"
    IFS=$'\037' read -r RESULT_VALUE RESULT_NAME RESULT_DETAIL RESULT_LOG_PATH < "$file"
}

wait_for_parallel_completion() {
    local status_dir="$1"
    local total="$2"
    local done=0
    local status_file

    while true; do
        done=0
        for status_file in "$status_dir"/*.status; do
            [[ -e "$status_file" ]] || continue
            if ! read_status_file "$status_file"; then
                continue
            fi
            if [[ "$STATUS_PHASE" == "done" || "$STATUS_PHASE" == "failed" ]]; then
                done=$((done + 1))
            fi
        done
        if [[ $done -ge $total ]]; then
            break
        fi
        sleep 0.2
    done
}

monitor_parallel_progress() {
    local status_dir="$1"
    local total="$2"
    local jobs="$3"
    if [[ -t 1 ]]; then
        uv run hacc-e2e-monitor --status-dir "$status_dir" --total "$total" --jobs "$jobs"
    else
        wait_for_parallel_completion "$status_dir" "$total"
    fi
}

run_workload_sequential() {
    local idx="$1"
    local yaml_input="$2"
    local total="$3"
    local yaml="$yaml_input"
    local basename
    local build_dir
    local failed=0
    local detail=""
    local verify_output=""

    basename=$(basename "$yaml_input" .yaml)
    build_dir="${WORKLOAD_BUILD_DIRS[$idx]}"
    if [[ $DRY_RUN -eq 0 ]]; then
        mkdir -p "$build_dir"
    fi

    echo ""
    info "[$((idx + 1))/$total] ${BOLD}$basename${NC}"
    info "  YAML:   $yaml_input"
    info "  Output: $build_dir"

    if ! yaml=$(resolve_yaml_path "$yaml_input"); then
        err "  Workload not found: $yaml_input"
        detail="workload not found"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("FAIL  $basename  (workload not found)")
        if [[ $DRY_RUN -eq 0 ]]; then
            write_persistent_result_for_build_dir "$build_dir" "FAIL" "$basename" "$detail" ""
        fi
        return
    fi

    if [[ $SKIP_COMPILE -eq 0 ]]; then
        step "  [1/4] Compiling firmware..."
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "  [DRY-RUN] hacc-compile $yaml -o $build_dir --dump-ir --opt-level s"
        else
            if uv run hacc-compile "$yaml" -o "$build_dir" --dump-ir --opt-level s 2>&1 | { if [[ $VERBOSE -eq 1 ]]; then cat; else tail -3; fi; }; then
                succ "  Compilation done."
            else
                err "  Compilation failed!"
                failed=1
                detail="compile"
            fi
        fi
    else
        info "  [1/4] Skipping compilation (--skip-compile)"
    fi

    if [[ $failed -eq 0 ]]; then
        step "  [2/4] Generating DRAM test data..."
        local ir_path="$build_dir/hardware_ir.json"
        if [[ ! -f "$ir_path" && $DRY_RUN -eq 0 ]]; then
            err "  hardware_ir.json not found — did compilation succeed with --dump-ir?"
            failed=1
            detail="hardware_ir missing"
        else
            if [[ $DRY_RUN -eq 1 ]]; then
                echo "  [DRY-RUN] python -m hybridacc_verify.gen.gen_test_dram --ir $ir_path --workload $yaml --output-dir $build_dir --seed $SEED"
            else
                if uv run python -m hybridacc_verify.gen.gen_test_dram --ir "$ir_path" --workload "$yaml" --output-dir "$build_dir" --seed "$SEED" 2>&1 | { if [[ $VERBOSE -eq 1 ]]; then cat; else tail -5; fi; }; then
                    succ "  Test data generated."
                else
                    err "  Test data generation failed!"
                    failed=1
                    detail="gen-test-dram"
                fi
            fi
        fi
    fi

    if [[ $failed -eq 0 && $SKIP_SIM -eq 0 ]]; then
        step "  [3/4] Running ESL simulation..."
        local sim_args=(--clock-period "$CLOCK_PERIOD_NS" --max-cycles "$MAX_CYCLES")
        if [[ $CORE_DEBUG -eq 1 ]]; then
            sim_args+=(--core-debug)
        fi
        if [[ $FAST_BOOT -eq 1 ]]; then
            sim_args+=(--fast-boot)
        fi
        if [[ $TRACE -eq 1 ]]; then
            sim_args+=(--trace "$build_dir/trace.json" --trace-level "$TRACE_LEVEL")
        fi
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "  [DRY-RUN] hybridacc_sim.sh run-task $build_dir ${sim_args[*]}"
        else
            if "$SCRIPT_DIR/hybridacc_sim.sh" run-task "$build_dir" "${sim_args[@]}" 2>&1 | { if [[ $VERBOSE -eq 1 ]]; then cat; else tail -5; fi; }; then
                succ "  Simulation done."
            else
                err "  Simulation failed or timed out!"
                failed=1
                detail="simulation"
            fi
        fi
    elif [[ $SKIP_SIM -eq 1 ]]; then
        info "  [3/4] Skipping simulation (--skip-sim)"
    fi

    if [[ $failed -eq 0 ]]; then
        step "  [4/5] Verifying output..."
        if [[ $DRY_RUN -eq 1 ]]; then
            echo "  [DRY-RUN] python -m hybridacc_verify.check.compare_golden $build_dir --tolerance $TOLERANCE"
        else
            verify_output=$(uv run python -m hybridacc_verify.check.compare_golden "$build_dir" --tolerance "$TOLERANCE" 2>&1) || failed=1
            echo "$verify_output" | sed 's/^/    /'
            if [[ $failed -ne 0 && -z "$detail" ]]; then
                detail="verification"
            fi
            if [[ $failed -eq 0 ]]; then
                succ "  ${BOLD}$basename${NC}: PASS"
                if [[ $TRACE -eq 1 && -f "$build_dir/trace.json" ]]; then
                    step "  [5/5] Analyzing trace..."
                    if uv run python "$TOP_DIR/python/trace_parser/analyze_trace.py" "$build_dir/trace.json" --csv "$build_dir/trace_analysis.csv" 2>&1 | sed 's/^/    /'; then
                        :
                    else
                        err "  Trace analysis failed!"
                        failed=1
                        detail="trace analysis"
                    fi
                fi
            fi
        fi
    fi

    if [[ $DRY_RUN -eq 1 ]]; then
        SKIP_COUNT=$((SKIP_COUNT + 1))
        RESULTS+=("SKIP  $basename  (dry-run)")
        return
    fi

    if [[ $failed -eq 0 ]]; then
        PASS_COUNT=$((PASS_COUNT + 1))
        RESULTS+=("PASS  $basename")
        write_persistent_result_for_build_dir "$build_dir" "PASS" "$basename" "" ""
    else
        if [[ -z "$detail" ]]; then
            detail="pipeline error"
        fi
        FAIL_COUNT=$((FAIL_COUNT + 1))
        RESULTS+=("FAIL  $basename  ($detail)")
        write_persistent_result_for_build_dir "$build_dir" "FAIL" "$basename" "$detail" ""
    fi
}

run_workload_worker() {
    local idx="$1"
    local yaml_input="$2"
    local total="$3"
    local yaml=""
    local basename
    local build_dir
    local task_log
    local result_file="$STATUS_DIR/$idx.result"
    local status_file="$STATUS_DIR/$idx.status"
    local detail=""

    basename=$(basename "$yaml_input" .yaml)
    build_dir="${WORKLOAD_BUILD_DIRS[$idx]}"
    task_log="$build_dir/e2e_run.log"
    mkdir -p "$build_dir"
    : > "$task_log"

    if ! yaml=$(resolve_yaml_path "$yaml_input"); then
        write_status_file "$status_file" "failed" "$basename" "workload not found"
        write_result_file "$result_file" "FAIL" "$basename" "workload not found" "$task_log"
        write_persistent_result_for_build_dir "$build_dir" "FAIL" "$basename" "workload not found" "$task_log"
        return 0
    fi

    if [[ $SKIP_COMPILE -eq 0 ]]; then
        write_status_file "$status_file" "compile" "$basename" "compiling firmware"
        if ! uv run hacc-compile "$yaml" -o "$build_dir" --dump-ir --opt-level s >> "$task_log" 2>&1; then
            detail="compile"
        fi
    fi

    if [[ -z "$detail" ]]; then
        write_status_file "$status_file" "gendram" "$basename" "generating DRAM data"
        local ir_path="$build_dir/hardware_ir.json"
        if [[ ! -f "$ir_path" ]]; then
            echo "hardware_ir.json not found at $ir_path" >> "$task_log"
            detail="hardware_ir missing"
        elif ! uv run python -m hybridacc_verify.gen.gen_test_dram --ir "$ir_path" --workload "$yaml" --output-dir "$build_dir" --seed "$SEED" >> "$task_log" 2>&1; then
            detail="gen-test-dram"
        fi
    fi

    if [[ -z "$detail" && $SKIP_SIM -eq 0 ]]; then
        local sim_args=(--clock-period "$CLOCK_PERIOD_NS" --max-cycles "$MAX_CYCLES")
        if [[ $CORE_DEBUG -eq 1 ]]; then
            sim_args+=(--core-debug)
        fi
        if [[ $FAST_BOOT -eq 1 ]]; then
            sim_args+=(--fast-boot)
        fi
        if [[ $TRACE -eq 1 ]]; then
            sim_args+=(--trace "$build_dir/trace.json" --trace-level "$TRACE_LEVEL")
        fi
        write_status_file "$status_file" "sim" "$basename" "running simulator"
        if ! "$SCRIPT_DIR/hybridacc_sim.sh" run-task "$build_dir" "${sim_args[@]}" >> "$task_log" 2>&1; then
            detail="simulation"
        fi
    fi

    if [[ -z "$detail" ]]; then
        write_status_file "$status_file" "verify" "$basename" "verifying output"
        if ! uv run python -m hybridacc_verify.check.compare_golden "$build_dir" --tolerance "$TOLERANCE" >> "$task_log" 2>&1; then
            detail="verification"
        fi
    fi

    if [[ -z "$detail" && $TRACE -eq 1 && -f "$build_dir/trace.json" ]]; then
        write_status_file "$status_file" "trace" "$basename" "analyzing trace"
        if ! uv run python "$TOP_DIR/python/trace_parser/analyze_trace.py" "$build_dir/trace.json" --csv "$build_dir/trace_analysis.csv" >> "$task_log" 2>&1; then
            detail="trace analysis"
        fi
    fi

    if [[ -z "$detail" ]]; then
        write_status_file "$status_file" "done" "$basename" "completed"
        write_result_file "$result_file" "PASS" "$basename" "" "$task_log"
        write_persistent_result_for_build_dir "$build_dir" "PASS" "$basename" "" "$task_log"
    else
        write_status_file "$status_file" "failed" "$basename" "$detail"
        write_result_file "$result_file" "FAIL" "$basename" "$detail" "$task_log"
        write_persistent_result_for_build_dir "$build_dir" "FAIL" "$basename" "$detail" "$task_log"
    fi
}

run_workloads_parallel() {
    local total="$1"
    local running_jobs=0
    local idx=0
    local basename
    local result_file
    local result name detail log_path

    STATUS_DIR=$(mktemp -d)
    for ((idx = 0; idx < total; idx++)); do
        basename=$(basename "${WORKLOADS[$idx]}" .yaml)
        write_status_file "$STATUS_DIR/$idx.status" "queued" "$basename" "waiting for worker slot"
    done

    monitor_parallel_progress "$STATUS_DIR" "$total" "$JOBS" &
    PROGRESS_MONITOR_PID=$!

    for ((idx = 0; idx < total; idx++)); do
        run_workload_worker "$idx" "${WORKLOADS[$idx]}" "$total" &
        running_jobs=$((running_jobs + 1))
        if [[ $running_jobs -ge $JOBS ]]; then
            wait -n || true
            running_jobs=$((running_jobs - 1))
        fi
    done

    while [[ $running_jobs -gt 0 ]]; do
        wait -n || true
        running_jobs=$((running_jobs - 1))
    done

    wait "$PROGRESS_MONITOR_PID" || true
    PROGRESS_MONITOR_PID=""
    echo ""

    for ((idx = 0; idx < total; idx++)); do
        result_file="$STATUS_DIR/$idx.result"
        if [[ ! -f "$result_file" ]]; then
            PASS_COUNT=$((PASS_COUNT + 0))
            FAIL_COUNT=$((FAIL_COUNT + 1))
            RESULTS+=("FAIL  worker_$idx  (missing result file)")
            continue
        fi
        if ! read_result_file "$result_file"; then
            PASS_COUNT=$((PASS_COUNT + 0))
            FAIL_COUNT=$((FAIL_COUNT + 1))
            RESULTS+=("FAIL  worker_$idx  (unreadable result file)")
            continue
        fi
        if [[ "$RESULT_VALUE" == "PASS" ]]; then
            PASS_COUNT=$((PASS_COUNT + 1))
            RESULTS+=("PASS  $RESULT_NAME")
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            RESULTS+=("FAIL  $RESULT_NAME  ($RESULT_DETAIL; log=$RESULT_LOG_PATH)")
        fi
    done
}

print_summary() {
    local total="$1"
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
        err "${BOLD}$FAIL_COUNT / $total test(s) FAILED.${NC} ($PASS_COUNT passed)"
    fi
}

cleanup_parallel_state() {
    if [[ -n "${PROGRESS_MONITOR_PID:-}" ]]; then
        kill "$PROGRESS_MONITOR_PID" >/dev/null 2>&1 || true
        wait "$PROGRESS_MONITOR_PID" 2>/dev/null || true
        PROGRESS_MONITOR_PID=""
    fi
    tput cnorm >/dev/null 2>&1 || true
    if [[ -n "${STATUS_DIR:-}" && -d "$STATUS_DIR" ]]; then
        rm -rf "$STATUS_DIR"
    fi
}

trap cleanup_parallel_state EXIT

# ── Process workloads ──────────────────────────────────────────────────────
TOTAL=${#WORKLOADS[@]}
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
RESUME_SKIP_COUNT=0
RESULTS=()
WORKLOAD_BUILD_DIRS=()

prepare_workloads_for_execution "$TOTAL"
ORIGINAL_TOTAL=$TOTAL
TOTAL=${#WORKLOADS[@]}

if [[ $JOBS -le 0 ]]; then
    if [[ $TOTAL -gt 1 && $DRY_RUN -eq 0 ]]; then
        JOBS=$(default_jobs)
    else
        JOBS=1
    fi
fi
if [[ $JOBS -gt $TOTAL ]]; then
    JOBS=$TOTAL
fi
if [[ $DRY_RUN -eq 1 ]]; then
    JOBS=1
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
if [[ $RESUME -eq 1 && $RESUME_SKIP_COUNT -gt 0 ]]; then
    info "Resume mode: reusing ${BOLD}$RESUME_SKIP_COUNT${NC} completed workload(s), ${BOLD}$TOTAL${NC} remaining"
fi
if [[ $RESUME -eq 1 ]]; then
    info "Processing ${BOLD}$TOTAL${NC} remaining workload(s) out of ${BOLD}$ORIGINAL_TOTAL${NC} total with seed=$SEED jobs=$JOBS"
else
    info "Processing ${BOLD}$TOTAL${NC} workload(s) with seed=$SEED jobs=$JOBS"
fi
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# ── Step 0: Ensure simulator is built ─────────────────────────────────────
if [[ $TOTAL -gt 0 && $SKIP_BUILD -eq 0 && $SKIP_SIM -eq 0 ]]; then
    step "Building ESL simulator..."
    run_cmd "$SCRIPT_DIR/hybridacc_sim.sh" build
    succ "Simulator built."
fi

if [[ $TOTAL -gt 1 && $JOBS -gt 1 && $VERBOSE -eq 1 ]]; then
    warn "Batch parallel mode writes detailed logs to each build directory's e2e_run.log; terminal shows the live progress dashboard only."
fi

if [[ $TOTAL -eq 0 ]]; then
    succ "No remaining workloads to run."
elif [[ $TOTAL -gt 1 && $JOBS -gt 1 && $DRY_RUN -eq 0 ]]; then
    run_workloads_parallel "$TOTAL"
else
    for ((idx = 0; idx < TOTAL; idx++)); do
        run_workload_sequential "$idx" "${WORKLOADS[$idx]}" "$TOTAL"
    done
fi

print_summary "$ORIGINAL_TOTAL"
exit $FAIL_COUNT
