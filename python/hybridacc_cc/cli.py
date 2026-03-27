"""CLI entry point for hybridacc-cc compiler.

Usage:
    hacc-compile workload.yaml -o output_dir/ [--compile] [--gcc PATH]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .frontend import parse_workload
from .lowering import lower_workload
from .codegen import generate_firmware
from .elf_builder import build_gcc_command, compile_firmware, validate_elf


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="hacc-compile",
        description="HybridAcc-CC: Workload YAML → Firmware C/ELF compiler",
    )
    parser.add_argument(
        "workload",
        type=Path,
        help="Path to the workload YAML file",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=Path("build"),
        help="Output directory for generated files (default: build/)",
    )
    parser.add_argument(
        "--kernel-dir",
        type=Path,
        default=None,
        help="Directory containing kernel JSON files "
             "(default: design/hybridacc-cc/kernel/json/)",
    )
    parser.add_argument(
        "--template-dir",
        type=Path,
        default=None,
        help="Directory containing Jinja2 templates "
             "(default: built-in templates)",
    )
    parser.add_argument(
        "--compile",
        action="store_true",
        help="Also invoke riscv32-unknown-elf-gcc to produce ELF",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print GCC command without executing (implies --compile)",
    )
    parser.add_argument(
        "--gcc",
        type=str,
        default="riscv32-unknown-elf-gcc",
        help="Path to RISC-V GCC cross compiler",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Post-link ELF size validation (requires --compile)",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output",
    )

    args = parser.parse_args(argv)

    # Stage 0: Parse YAML
    if args.verbose:
        print(f"[Stage 0] Parsing {args.workload}")
    workload_ir = parse_workload(args.workload)

    # Stage 1: Operator Lowering
    if args.verbose:
        print(f"[Stage 1] Lowering {len(workload_ir.ops)} operators")
    hardware_ir = lower_workload(workload_ir)

    # Stage 2 + 3: Code Generation (includes PE payload)
    if args.verbose:
        print(f"[Stage 2+3] Generating firmware to {args.output}")
    generated = generate_firmware(
        hardware_ir,
        args.output,
        kernel_json_dir=args.kernel_dir,
        template_dir=args.template_dir,
    )
    for f in generated:
        if args.verbose:
            print(f"  → {f}")

    # Stage 4: ELF Compilation (optional)
    if args.compile or args.dry_run:
        if args.dry_run:
            cmd = build_gcc_command(args.output, gcc=args.gcc)
            print(" ".join(cmd))
        else:
            if args.verbose:
                print("[Stage 4] Compiling firmware ELF")
            elf_path = compile_firmware(args.output, gcc=args.gcc)
            print(f"ELF: {elf_path}")

            if args.validate:
                size_tool = args.gcc.replace("gcc", "size")
                sizes = validate_elf(elf_path, size_tool=size_tool)
                print(f"  .text={sizes['text']}B  "
                      f".data={sizes['data']}B  "
                      f".bss={sizes['bss']}B  "
                      f"total={sizes['total_dec']}B")
    else:
        print(f"Generated {len(generated)} files in {args.output}/")
        if args.verbose:
            cmd = build_gcc_command(args.output, gcc=args.gcc)
            print(f"To compile: {' '.join(cmd)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
