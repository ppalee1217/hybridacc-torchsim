#!/bin/bash
usage() {
    echo -e "\033[1;32mUsage:\033[0m $0 [\033[1;33mOPTION\033[0m] [\033[1;33mTB_NAME\033[0m]"
    echo -e "  \033[1;33mOPTION\033[0m:"
    echo -e "    \033[1;33m-d, --dir DIR\033[0m   Specify the testbench top directory. (default: testbench/noc)"
    echo -e "    \033[1;33m-l, --list\033[0m        List all testbench directories under testbench top directory."
    echo -e "    \033[1;33m-a, --all\033[0m         Generate for all testbench directories under testbench top directory."
    echo -e "    \033[1;33m-h, --help\033[0m        Show this help."
    echo -e "    \033[1;33m-o, --out-dir DIR\033[0m Specify top output directory. (default: ./output/noc-sim)"
    echo -e "    \033[1;33mTB_NAME\033[0m         Generate for the specified testbench directory."
    exit 1
}

TOP_DIR="testbench/noc"  # Default testbench top directory
OUT_ROOT="./output/noc-sim"  # Default top output directory
LIST=false
ALL=false

# Parse command-line arguments
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
        -o|--out-dir)
            if [ -z "$2" ]; then
                echo -e "\033[1;31mError:\033[0m Missing argument for $1."
                exit 1
            fi
            OUT_ROOT="$2"
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
        --) shift; break;;
        -*)
            echo -e "\033[1;31mError:\033[0m Unknown option '$1'."
            usage
            ;;
        *) break ;;
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
    local OUT_DIR="$OUT_ROOT/$TB_NAME"

    if [ ! -d "$TB_DIR" ]; then
        echo -e "\033[1;31mError:\033[0m Testbench directory '$TB_DIR' does not exist."
        exit 1
    fi

    echo "Processing $TB_NAME..."

    # Step 1: Generate test data (use uv if available)
    if command -v uv >/dev/null 2>&1; then
        uv run python -m hybridacc_verify.main gen-noc --config "$TB_DIR/config.json"
    else
        python -m hybridacc_verify.main gen-noc --config "$TB_DIR/config.json"
    fi

    # Step 2: Generate PE ASM binary
    ASSEMBLER="design/hybridacc-pe-isa/tools/bin/ha-asm"
    if [ ! -x "$ASSEMBLER" ]; then
        if command -v ha-asm >/dev/null 2>&1; then
            ASSEMBLER="$(command -v ha-asm)"
        else
            echo -e "\033[1;31mError:\033[0m Assembler not found at '$ASSEMBLER' and 'ha-asm' not in PATH."
            exit 1
        fi
    fi

    mkdir -p "$OUT_DIR"
    "$ASSEMBLER" "$TB_DIR/pe_program.asm" -o "$OUT_DIR/pe_program.bin"

    # Step 3: If there are split simulation parts, copy pe_program.bin to each part directory
    for part_dir in "$OUT_DIR"/*/; do
        if [ -d "$part_dir" ]; then
            cp -f "$OUT_DIR/pe_program.bin" "$part_dir"
        fi
    done
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
    echo "Generating for all testbench directories under '$TOP_DIR' (outputs -> '$OUT_ROOT')..."
    for dir in "$TOP_DIR"/*/; do
        [ -d "$dir" ] && generate_testbench "$(basename "$dir")"
    done
    exit 0
fi

# If neither list nor all, expect a TB_NAME
if [ -z "$TB_NAME" ]; then
    usage
fi

generate_testbench "$TB_NAME"
