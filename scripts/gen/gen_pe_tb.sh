#!/bin/bash

usage() {
    echo -e "\033[1;32mUsage:\033[0m $0 [\033[1;33mOPTION\033[0m] [\033[1;33mTB_NAME\033[0m]"
    echo -e "  \033[1;33mOPTION\033[0m:"
    echo -e "    \033[1;33m-l, --list\033[0m   List all testbench directories under testbench/pe."
    echo -e "    \033[1;33m-a, --all\033[0m    Generate for all testbench directories under testbench/pe."
    echo -e "    \033[1;33mTB_NAME\033[0m      Generate for the specified testbench directory."
    echo -e "           If TB_NAME does not exist, the script will abort."
    exit 1
}

list_testbenches() {
    echo -e "\033[1;32mAvailable testbench directories:\033[0m"
    for dir in testbench/pe/*/; do
        if [ -d "$dir" ]; then
            echo "  - $(basename "$dir")"
        fi
    done
}

generate_testbench() {
    local TB_NAME=$1
    if [ ! -d "testbench/pe/$TB_NAME" ]; then
        echo -e "\033[1;31mError:\033[0m Testbench directory 'testbench/pe/$TB_NAME' does not exist."
        exit 1
    fi

    echo "Processing $TB_NAME..."

    # Step 1: Generate test data
    uv run python -m hybridacc_verify.main gen-pe --config testbench/pe/$TB_NAME/config.json

    # Step 2: Generate PE ASM binary
    ASSEMBLER=design/hybridacc-pe-isa/tools/bin/ha-asm
    $ASSEMBLER testbench/pe/$TB_NAME/pe_program.asm -o ./output/pe-sim/$TB_NAME/pe_program.bin --json ./output/pe-sim/$TB_NAME/pe_program.json
}

TB_NAME=$1

if [ -z "$TB_NAME" ]; then
    usage
elif [ "$TB_NAME" == "-h" ] || [ "$TB_NAME" == "--help" ]; then
    usage
elif [ "$TB_NAME" == "-l" ] || [ "$TB_NAME" == "--list" ]; then
    list_testbenches
    exit 0
elif [ "$TB_NAME" == "-a" ] || [ "$TB_NAME" == "--all" ]; then
    echo "Generating for all testbench directories..."
    for dir in testbench/pe/*/; do
        if [ -d "$dir" ]; then
            generate_testbench "$(basename "$dir")"
        fi
    done
else
    generate_testbench "$TB_NAME"
fi
