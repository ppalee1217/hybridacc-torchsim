#!/bin/bash
set -euo pipefail

ASM_DIR="asm/template"
ASSEMBLER="./tools/bin/ha-asm"
OBJDUMP="./tools/bin/ha-objdump"
OUTPUT_DIR="output"

if [ ! -d "$ASM_DIR" ]; then
    echo "Directory $ASM_DIR does not exist."
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

shopt -s nullglob
asm_files=("$ASM_DIR"/*.asm)
if [ ${#asm_files[@]} -eq 0 ]; then
    echo "No .asm files found in $ASM_DIR."
    exit 0
fi

for asm_file in "${asm_files[@]}"; do
    base="$(basename "$asm_file" .asm)"
    bin_file="$OUTPUT_DIR/$base.bin"
    hex_file="$OUTPUT_DIR/$base.hex"
    json_file="$OUTPUT_DIR/$base.json"

    echo "[INFO] Assembling $asm_file -> $bin_file"
    if ! "$ASSEMBLER" "$asm_file" -o "$bin_file" --hex "$hex_file" --json "$json_file"; then
        echo "Failed to compile $asm_file."
        exit 1
    fi

    # (可選) 產生反組譯
    if [ -x "$OBJDUMP" ]; then
        "$OBJDUMP" "$bin_file" -t > "$OUTPUT_DIR/$base.disasm" || true
    fi
done

echo "All .asm files compiled successfully."