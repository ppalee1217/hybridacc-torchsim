#!/usr/bin/env bash
# pe_sim.sh [run|build|clean|run-all]

MODE=$1
TB_NAME="conv_k3c4"  # Default testbench name

# if $2 is provided, use it as TB_NAME
if [ ! -z "$2" ]; then
    TB_NAME=$2
fi

TOP_DIR=$(pwd)
OUTPUT_DIR="$TOP_DIR/output"
SIM_MODEL_DIR="$TOP_DIR/design/hybridacc-ESL"
BUILD_DIR="$SIM_MODEL_DIR/build/pe_sim"
# SIM_DATA_DIR default is under output/pe-sim/<tb>
SIM_DATA_BASE="$OUTPUT_DIR/pe-sim"
CMAKEFILE_DIR="$SIM_MODEL_DIR/test"

# Pretty print helpers and ensure output dirs exist
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
  pe_sim.sh clean
  pe_sim.sh build
  pe_sim.sh run [-d tb_dir] tb_name
  pe_sim.sh run-all [-d tb_dir]

Examples:
  pe_sim.sh run -d output/pe-sim conv_k3c4
  pe_sim.sh run-all -d output/pe-sim
EOF
}

# Run single TB: args: tb_name tb_dir_base
run_single_tb() {
    local tb="$1"
    local tb_dir_base="$2"
    local sim_data_dir="$tb_dir_base/$tb"

    # Check if simulation data exists (e.g., meta.txt or binaries)
    if [ ! -d "$sim_data_dir" ]; then
        err "Simulation data directory '$sim_data_dir' not found."
        return 1
    fi

    if [ ! -x "$BUILD_DIR/test_pe_sim" ]; then
        err "Executable $BUILD_DIR/test_pe_sim not found or not executable. Please run: pe_sim.sh build"
        return 2
    fi

    local out_log="$OUTPUT_DIR/out-pe-$tb.log"
    info "Running PE TB='$tb'"
    info "  sim_data='$sim_data_dir'"
    info "  log: $out_log"

    # Note: test_pe_sim needs to be updated to accept data directory as argument
    # We pass it here assuming the C++ executable supports it (or will support it)
    "$BUILD_DIR"/test_pe_sim "$sim_data_dir" > "$out_log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        err "Simulation failed for '$tb' (rc=$rc). See $out_log"
        return $rc
    fi
    succ "Simulation completed for '$tb'. Log: $out_log"
    return 0
}

# Parse modes
if [ "$MODE" == "run" ]; then
    # shift off MODE
    shift
    # parse optional -d
    TB_DIR="$SIM_DATA_BASE"
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
            -*)
                echo "Unknown option: $1"
                usage
                exit 1
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

    run_single_tb "$TB_NAME" "$TB_DIR"

elif [ "$MODE" == "run-all" ]; then
    # shift off MODE
    shift
    TB_DIR="$SIM_DATA_BASE"
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
            -*)
                echo "Unknown option: $1"
                usage
                exit 1
                ;;
            *)
                echo "Unknown argument: $1"
                usage
                exit 1
                ;;
        esac
    done

    if [ ! -d "$TB_DIR" ]; then
        err "TB directory '$TB_DIR' does not exist."
        exit 1
    fi

    # For PE sim, we usually iterate over subdirectories in the TB_DIR
    shopt -s nullglob

    total=0
    succ_count=0
    fail_count=0

    # enumerate subdirectories
    for d in "$TB_DIR"/*/; do
        [ -d "$d" ] || continue
        tbname=$(basename "$d")
        run_single_tb "$tbname" "$TB_DIR"
        rc=$?
        total=$((total+1))
        if [ $rc -eq 0 ]; then
            succ_count=$((succ_count+1))
        else
            fail_count=$((fail_count+1))
            info "Continuing to next TB..."
        fi
    done

    info "Run-all summary: total=$total succeeded=$succ_count failed=$fail_count"

elif [ "$MODE" == "build" ]; then
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    # Options for debug levels if needed
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_PE_COMPONENTS $CMAKEFILE_DIR
    # cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=DEBUG_LEVEL_PE_STAGE $CMAKEFILE_DIR
    cmake -DENABLE_DEBUG_UTILS=OFF $CMAKEFILE_DIR
    make test_pe_sim -j8
    cd ..
elif [ "$MODE" == "clean" ]; then
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
    fi
else
    usage
fi
