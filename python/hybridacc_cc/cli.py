"""CLI entry point for hybridacc-cc compiler.

Usage:
    hacc-compile workload.yaml -o output_dir/ [--no-compile] [--gcc PATH]
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

from . import __version__
from .frontend import parse_workload
from .lowering import lower_workload
from .codegen import generate_firmware
from .elf_builder import DEFAULT_MARCH, build_gcc_command, compile_firmware, validate_elf


def _dump_ir(obj, path: Path) -> None:
    """Serialize a dataclass IR to JSON."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(dataclasses.asdict(obj), indent=2, default=str),
        encoding="utf-8",
    )


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
        "--dump-ir",
        action="store_true",
        help="Dump intermediate IR (WorkloadIR, HardwareIR) as JSON",
    )
    parser.add_argument(
        "--no-compile",
        action="store_true",
        help="Only generate C source files, skip GCC compilation",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print GCC command without executing (implies compilation)",
    )
    parser.add_argument(
        "--gcc",
        type=str,
        default="riscv32-unknown-elf-gcc",
        help="Path to RISC-V GCC cross compiler",
    )
    parser.add_argument(
        "--stack-size",
        type=int,
        default=4096,
        help="Stack reserved size in bytes (default: 4096)",
    )
    parser.add_argument(
        "--opt-level",
        type=str,
        choices=["0", "1", "2", "s"],
        default="2",
        help="GCC optimization level (default: 2)",
    )
    parser.add_argument(
        "--march",
        type=str,
        default=DEFAULT_MARCH,
        help=f"RISC-V march string (default: {DEFAULT_MARCH})",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Post-link ELF size validation",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output",
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )

    args = parser.parse_args(argv)

    # Stage 0: Parse YAML
    if args.verbose:
        print(f"[Stage 0] Parsing {args.workload}")
    workload_ir = parse_workload(args.workload)

    if args.dump_ir:
        ir_path = args.output / "workload_ir.json"
        _dump_ir(workload_ir, ir_path)
        if args.verbose:
            print(f"  → {ir_path}")

    # Stage 1: Operator Lowering
    if args.verbose:
        print(f"[Stage 1] Lowering {len(workload_ir.ops)} operators")
    hardware_ir = lower_workload(workload_ir)

    if args.dump_ir:
        ir_path = args.output / "hardware_ir.json"
        _dump_ir(hardware_ir, ir_path)
        if args.verbose:
            print(f"  → {ir_path}")

    # Stage 2 + 3: Code Generation (includes PE payload)
    if args.verbose:
        print(f"[Stage 2+3] Generating firmware to {args.output}")
    generated = generate_firmware(
        hardware_ir,
        args.output,
        kernel_json_dir=args.kernel_dir,
        template_dir=args.template_dir,
        stack_size=args.stack_size,
        march=args.march,
    )
    for f in generated:
        if args.verbose:
            print(f"  → {f}")

    # Stage 4: ELF Compilation (default unless --no-compile)
    do_compile = not args.no_compile
    if do_compile or args.dry_run:
        opt = f"-O{args.opt_level}"
        if args.dry_run:
            cmd = build_gcc_command(
                args.output, gcc=args.gcc, march=args.march, opt_level=opt,
            )
            print(" ".join(cmd))
        else:
            if args.verbose:
                print("[Stage 4] Compiling firmware ELF")
            elf_path = compile_firmware(
                args.output, gcc=args.gcc, march=args.march, opt_level=opt,
            )
            print(f"ELF: {elf_path}")

            if args.validate:
                size_tool = args.gcc.replace("gcc", "size")
                sizes = validate_elf(
                    elf_path,
                    stack_size=args.stack_size,
                    size_tool=size_tool,
                )
                print(f"  .text={sizes['text']}B  "
                      f".data={sizes['data']}B  "
                      f".bss={sizes['bss']}B  "
                      f"total={sizes['total_dec']}B")
    else:
        print(f"Generated {len(generated)} files in {args.output}/")
        if args.verbose:
            cmd = build_gcc_command(args.output, gcc=args.gcc, march=args.march)
            print(f"To compile: {' '.join(cmd)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
