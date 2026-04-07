#!/usr/bin/env bash
# cluster_sim.sh [run|build|clean|run-all]

MODE=$1
TB_NAME=""

# if $2 is provided, use it as TB_NAME
if [ ! -z "$2" ]; then
    TB_NAME=$2
fi

# Resolve project root based on this script location (robust to caller CWD)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOP_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL"
BUILD_DIR="$SIM_MODEL_DIR/build/cluster_sim"

# Default directory for generated testbenches
SIM_DATA_BASE="$OUTPUT_DIR/cluster-sim"
CMAKEFILE_DIR="$SIM_MODEL_DIR/test"

# Static/default simulation parameters
CLOCK_PERIOD=1
TIMEOUT_CYCLES=200

# Pretty print helpers
RED=$(printf '\033[0;31m')
GREEN=$(printf '\033[0;32m')
YELLOW=$(printf '\033[0;33m')
BLUE=$(printf '\033[0;34m')
BOLD=$(printf '\033[1m')
NC=$(printf '\033[0m')

info() { printf "%s [INFO]  %s\n" "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
warn() { printf "%s [WARN]  %s\n" "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }
err()  { printf "%s [ERROR] %s\n" "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >&2; }
succ() { printf "%s [OK]    %s\n" "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

mkdir -p "$OUTPUT_DIR" "$SIM_DATA_BASE"

usage() {
    cat <<EOF
Usage:
  cluster_sim.sh clean
  cluster_sim.sh build
        cluster_sim.sh run [-d tb_dir] [-v] [-f] [--trace-file path] [--clock-period ns] [--timeout-cycles cycles] [--advanced] tb_name
        cluster_sim.sh run-all [-d tb_dir] [-v] [-f] [--trace-file path] [--clock-period ns] [--timeout-cycles cycles] [--advanced]

Examples:
    cluster_sim.sh run -d output/cluster-sim conv_k3c4
    cluster_sim.sh run -v -d output/cluster-sim conv_k3c4
    cluster_sim.sh run -f -d output/cluster-sim conv_k3c4
    cluster_sim.sh run -f --trace-file output/trace-cluster.json -d output/cluster-sim conv_k3c4
    cluster_sim.sh run --clock-period 5 --timeout-cycles 50000 -d output/cluster-sim conv_k3c4
    cluster_sim.sh run --advanced -d output/cluster-sim conv_k3c4
    cluster_sim.sh run-all -d output/cluster-sim
EOF
}

# Run single TB: args: tb_name tb_dir_base
run_single_tb() {
    local tb="$1"
    local tb_dir_base="$2"
    local run_mode_flag="$3"
    local verbose_flag="$4"
    local clock_period="$5"
    local timeout_cycles="$6"
    local trace_enable="$7"
    local trace_file_hint="$8"
    local advanced_mode="$9"
    local trace_file=""
    local test_bin="test_cluster_sim"

    if [ "$advanced_mode" == "1" ]; then
        test_bin="test_cluster_sim_advanced"
    fi

    # Check if we are pointing to a specific tb directory or a base directory
    # If tb is in tb_dir_base, adjust path
    local sim_data_dir="$tb_dir_base/$tb"

    # If the user passed a path that already includes the test name or is absolute, handle it
    if [ ! -d "$sim_data_dir" ]; then
        # Try checking if tb_dir_base itself is the test dir
        if [ -f "$tb_dir_base/config.json" ] || [ -f "$tb_dir_base/config.txt" ]; then
            sim_data_dir="$tb_dir_base"
            # In this case tb name is just for logging
        elif [ -d "$tb" ]; then
             sim_data_dir="$tb"
        fi
    fi

    if [ ! -d "$sim_data_dir" ]; then
        err "Test directory not found: $sim_data_dir"
        return 1
    fi

    if [ ! -f "$sim_data_dir/config.json" ]; then
        err "config.json not found in $sim_data_dir"
        return 1
    fi

    if [ ! -x "$BUILD_DIR/$test_bin" ]; then
        err "Executable $BUILD_DIR/$test_bin not found. Please run: cluster_sim.sh build"
        return 2
    fi

    local out_log="$OUTPUT_DIR/out-cluster-$tb.log"

    if [ "$trace_enable" == "1" ]; then
        if [ -n "$trace_file_hint" ]; then
            trace_file="$trace_file_hint"
        else
            local trace_candidates=(
                "$tb_dir_base/trace-cluster-$tb.json"
                "$OUTPUT_DIR/trace-cluster-$tb.json"
                "$TOP_DIR/output/trace-cluster-$tb.json"
            )
            for p in "${trace_candidates[@]}"; do
                if [ -f "$p" ]; then
                    trace_file="$p"
                    break
                fi
            done
            if [ -z "$trace_file" ]; then
                trace_file="${trace_candidates[-1]}"
            fi
        fi
    fi

    info "Running Cluster TB='$tb'"
    if [ "$trace_enable" == "1" ]; then
        info "  trace='$trace_file'"
    else
        info "  trace=disabled"
    fi
    info "  sim_data='$sim_data_dir'"
    info "  clock_period=${clock_period}ns timeout_cycles=${timeout_cycles}"
    info "  log: $out_log"

    local cmd=("$BUILD_DIR/$test_bin"
        -d "$sim_data_dir"
        "$run_mode_flag"
        --clock-period "$clock_period"
        --timeout-cycles "$timeout_cycles")

    if [ -n "$verbose_flag" ]; then
        cmd+=("$verbose_flag")
    fi
    if [ "$trace_enable" == "1" ]; then
        cmd+=(-f "$trace_file")
    fi

    "${cmd[@]}" > "$out_log" 2>&1
    local rc=$?

    # Check for success pattern in log if rc is 0, or rely on rc
    if [ $rc -ne 0 ]; then
        err "Simulation failed for '$tb' (rc=$rc). See $out_log"
        tail -n 10 "$out_log" | sed 's/^/    /'
        return $rc
    fi

    # Double check log for explicit success message if needed, but rc should safeguard
    succ "Simulation completed for '$tb'. Log: $out_log"
    return 0
}

# Parse modes
if [ "$MODE" == "run" ]; then
    shift
    TB_DIR="$SIM_DATA_BASE"
    RUN_MODE_FLAG="--no-dry-run"
    VERBOSE_FLAG=""
    TRACE_ENABLE=0
    TRACE_FILE=""
    CLOCK_PERIOD_ARG="$CLOCK_PERIOD"
    TIMEOUT_CYCLES_ARG="$TIMEOUT_CYCLES"
    ADVANCED_MODE=0

    while [ $# -gt 0 ]; do
        case "$1" in
            -d)
                if [ -z "$2" ]; then
                    echo "Error: -d requires an argument"
                    usage
                    exit 1
                fi
                TB_DIR="$2"
                shift 2
                ;;
            -v)
                VERBOSE_FLAG="--verbose"
                shift
                ;;
            -f)
                TRACE_ENABLE=1
                shift
                ;;
            --trace-file)
                if [ -z "$2" ]; then
                    echo "Error: --trace-file requires an argument"
                    usage
                    exit 1
                fi
                TRACE_ENABLE=1
                TRACE_FILE="$2"
                shift 2
                ;;
            --clock-period)
                if [ -z "$2" ]; then
                    echo "Error: --clock-period requires an argument"
                    usage
                    exit 1
                fi
                CLOCK_PERIOD_ARG="$2"
                shift 2
                ;;
            --timeout-cycles)
                if [ -z "$2" ]; then
                    echo "Error: --timeout-cycles requires an argument"
                    usage
                    exit 1
                fi
                TIMEOUT_CYCLES_ARG="$2"
                shift 2
                ;;
            --advanced)
                ADVANCED_MODE=1
                shift
                ;;
            *)
                TB_NAME="$1"
                shift
                ;;
        esac
    done

    if [ -z "$TB_NAME" ]; then
        echo "Missing tb_name"
        usage
        exit 1
    fi

    if [[ "$TB_DIR" != /* ]] && [ -d "$TOP_DIR/$TB_DIR" ]; then
        TB_DIR="$TOP_DIR/$TB_DIR"
    fi

    run_single_tb "$TB_NAME" "$TB_DIR" "$RUN_MODE_FLAG" "$VERBOSE_FLAG" "$CLOCK_PERIOD_ARG" "$TIMEOUT_CYCLES_ARG" "$TRACE_ENABLE" "$TRACE_FILE" "$ADVANCED_MODE"

elif [ "$MODE" == "run-all" ]; then
    shift
    TB_DIR="$SIM_DATA_BASE"
    RUN_MODE_FLAG="--no-dry-run"
    VERBOSE_FLAG=""
    TRACE_ENABLE=0
    TRACE_FILE=""
    CLOCK_PERIOD_ARG="$CLOCK_PERIOD"
    TIMEOUT_CYCLES_ARG="$TIMEOUT_CYCLES"
    ADVANCED_MODE=0

    while [ $# -gt 0 ]; do
        case "$1" in
            -d)
                if [ -z "$2" ]; then
                    echo "Error: -d requires an argument"
                    usage
                    exit 1
                fi
                TB_DIR="$2"
                shift 2
                ;;
            -v)
                VERBOSE_FLAG="--verbose"
                shift
                ;;
            -f)
                TRACE_ENABLE=1
                shift
                ;;
            --trace-file)
                if [ -z "$2" ]; then
                    echo "Error: --trace-file requires an argument"
                    usage
                    exit 1
                fi
                TRACE_ENABLE=1
                TRACE_FILE="$2"
                shift 2
                ;;
            --clock-period)
                if [ -z "$2" ]; then
                    echo "Error: --clock-period requires an argument"
                    usage
                    exit 1
                fi
                CLOCK_PERIOD_ARG="$2"
                shift 2
                ;;
            --timeout-cycles)
                if [ -z "$2" ]; then
                    echo "Error: --timeout-cycles requires an argument"
                    usage
                    exit 1
                fi
                TIMEOUT_CYCLES_ARG="$2"
                shift 2
                ;;
            --advanced)
                ADVANCED_MODE=1
                shift
                ;;
            *)
                echo "Unknown argument: $1"
                usage
                exit 1
                ;;
        esac
    done

    if [[ "$TB_DIR" != /* ]] && [ -d "$TOP_DIR/$TB_DIR" ]; then
        TB_DIR="$TOP_DIR/$TB_DIR"
    fi

    if [ ! -d "$TB_DIR" ]; then
        err "TB directory '$TB_DIR' does not exist."
        exit 1
    fi

    total=0
    succ_count=0
    fail_count=0

    # Enumerate subdirectories
    shopt -s nullglob
    for d in "$TB_DIR"/*/; do
        [ -d "$d" ] || continue
        # cleanup path
        d=${d%/}
        tbname=$(basename "$d")

        # Check if it looks like a test dir (prefer config.json, keep config.txt compatibility)
        if [ -f "$d/config.json" ] || [ -f "$d/config.txt" ]; then
            local_trace_file=""
            if [ "$TRACE_ENABLE" == "1" ] && [ -n "$TRACE_FILE" ]; then
                base="${TRACE_FILE%.*}"
                ext="${TRACE_FILE##*.}"
                if [ "$base" = "$TRACE_FILE" ]; then
                    local_trace_file="${TRACE_FILE}-${tbname}"
                else
                    local_trace_file="${base}-${tbname}.${ext}"
                fi
            fi
            run_single_tb "$tbname" "$TB_DIR" "$RUN_MODE_FLAG" "$VERBOSE_FLAG" "$CLOCK_PERIOD_ARG" "$TIMEOUT_CYCLES_ARG" "$TRACE_ENABLE" "$local_trace_file" "$ADVANCED_MODE"
            rc=$?
            total=$((total+1))
            if [ $rc -eq 0 ]; then
                succ_count=$((succ_count+1))
            else
                fail_count=$((fail_count+1))
            fi
        fi
    done

    info "Run-all summary: total=$total succeeded=$succ_count failed=$fail_count"

    if [ $total -eq 0 ]; then
        err "No valid test directories found under '$TB_DIR' (expected config.json/config.txt)."
        exit 1
    fi

    if [ $fail_count -ne 0 ]; then
        err "Run-all finished with failures."
        exit 1
    fi

elif [ "$MODE" == "build" ]; then
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    # Enable Debug Utils if needed, or set to OFF for performance
    cmake -DENABLE_DEBUG_UTILS=OFF $CMAKEFILE_DIR
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_CLUSTER_COMPONENTS $CMAKEFILE_DIR
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_NOC_COMPONENTS $CMAKEFILE_DIR
    # Build both normal and advanced cluster simulation executables
    make test_cluster_sim test_cluster_sim_advanced -j8
    cd ..
    succ "Build complete: $BUILD_DIR/test_cluster_sim and $BUILD_DIR/test_cluster_sim_advanced"

elif [ "$MODE" == "clean" ]; then
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
    succ "Cleaned build directory."

else
    usage
fi
