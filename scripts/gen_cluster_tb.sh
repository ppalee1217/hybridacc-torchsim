#!/bin/bash

usage() {
    echo -e "\033[1;32mUsage:\033[0m $0 [\033[1;33mOPTION\033[0m] [\033[1;33mTB_NAME\033[0m]"
    echo -e "  \033[1;33mOPTION\033[0m:"
    echo -e "    \033[1;33m-d, --dir DIR\033[0m   Specify testbench top directory. (default: testbench/cluster)"
    echo -e "    \033[1;33m-l, --list\033[0m      List testbench directories under top directory."
    echo -e "    \033[1;33m-a, --all\033[0m       Generate all testbench workloads under top directory."
    echo -e "    \033[1;33m-h, --help\033[0m      Show this help."
    exit 1
}

TOP_DIR="testbench/cluster"
LIST=false
ALL=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--dir)
            if [ -z "$2" ]; then
                echo -e "\033[1;31mError:\033[0m Missing argument for $1."
                exit 1
            fi
            TOP_DIR="$2"
            shift 2
            ;;
        -l|--list)
            LIST=true
            shift
            ;;
        -a|--all)
            ALL=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo -e "\033[1;31mError:\033[0m Unknown option '$1'."
            usage
            ;;
        *)
            break
            ;;
    esac
done

list_testbenches() {
    if [ ! -d "$TOP_DIR" ]; then
        echo -e "\033[1;31mError:\033[0m TOP_DIR '$TOP_DIR' does not exist."
        exit 1
    fi
    echo -e "\033[1;32mAvailable testbench directories under '$TOP_DIR':\033[0m"
    for dir in "$TOP_DIR"/*/; do
        [ -d "$dir" ] && echo "  - $(basename "$dir")"
    done
}

generate_testbench() {
    local TB_NAME="$1"
    local TB_DIR="$TOP_DIR/$TB_NAME"

    if [ ! -d "$TB_DIR" ]; then
        echo -e "\033[1;31mError:\033[0m Testbench directory '$TB_DIR' does not exist."
        exit 1
    fi

    if [ ! -f "$TB_DIR/config.json" ]; then
        echo -e "\033[1;31mError:\033[0m Missing config.json under '$TB_DIR'."
        exit 1
    fi

    echo "Processing $TB_NAME..."
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    ASSEMBLER="$SCRIPT_DIR/../design/hybridacc-pe-isa/tools/bin/ha-asm"
    if command -v uv >/dev/null 2>&1; then
        uv run python -m hybridacc_verify.main gen-cluster --config "$TB_DIR/config.json" --assembler "$ASSEMBLER"
    else
        python -m hybridacc_verify.main gen-cluster --config "$TB_DIR/config.json" --assembler "$ASSEMBLER"
    fi
}

TB_NAME="$1"

if [ "$LIST" = true ]; then
    list_testbenches
    exit 0
fi

if [ "$ALL" = true ]; then
    if [ ! -d "$TOP_DIR" ]; then
        echo -e "\033[1;31mError:\033[0m TOP_DIR '$TOP_DIR' does not exist."
        exit 1
    fi
    echo "Generating all cluster workloads under '$TOP_DIR'..."
    for dir in "$TOP_DIR"/*/; do
        [ -d "$dir" ] && generate_testbench "$(basename "$dir")"
    done
    exit 0
fi

if [ -z "$TB_NAME" ]; then
    usage
fi

generate_testbench "$TB_NAME"
