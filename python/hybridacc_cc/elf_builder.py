"""Stage 4: ELF Compilation & Linking.

Assembles the GCC cross-compilation command and optionally invokes it.
Also provides post-link ELF validation.
"""

from __future__ import annotations

import subprocess
import re
from pathlib import Path
from typing import List, Optional


# Default GCC prefix for RISC-V bare-metal toolchain
DEFAULT_GCC = "riscv32-unknown-elf-gcc"


def build_gcc_command(
    output_dir: Path,
    elf_name: str = "firmware.elf",
    gcc: str = DEFAULT_GCC,
    march: str = "rv32i_zicsr",
    mabi: str = "ilp32",
    opt_level: str = "-O2",
) -> List[str]:
    """Build the complete GCC cross-compilation command.

    Returns the command as a list of strings (suitable for subprocess).
    """
    sources = ["firmware_main.c", "firmware_data.c", "firmware_ops.c"]
    source_paths = [str(output_dir / s) for s in sources]
    linker_script = str(output_dir / "linker.ld")
    elf_path = str(output_dir / elf_name)

    cmd = [
        gcc,
        f"-march={march}",
        f"-mabi={mabi}",
        "-nostdlib",
        "-ffreestanding",
        opt_level,
        "-Wall", "-Wextra",
        "-Wl,--gc-sections",
        "-ffunction-sections", "-fdata-sections",
        f"-T{linker_script}",
        f"-I{output_dir}",
        "-o", elf_path,
    ] + source_paths

    return cmd


def compile_firmware(
    output_dir: Path,
    elf_name: str = "firmware.elf",
    gcc: str = DEFAULT_GCC,
    dry_run: bool = False,
) -> Path:
    """Compile firmware C sources into an ELF binary.

    Args:
        output_dir: Directory containing generated C sources.
        elf_name: Output ELF filename.
        gcc: Path to the RISC-V GCC compiler.
        dry_run: If True, print command but don't execute.

    Returns:
        Path to the generated ELF file.
    """
    cmd = build_gcc_command(output_dir, elf_name, gcc)
    elf_path = output_dir / elf_name

    if dry_run:
        print(" ".join(cmd))
        return elf_path

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"GCC compilation failed (exit {result.returncode}):\n"
            f"STDOUT:\n{result.stdout}\n"
            f"STDERR:\n{result.stderr}"
        )

    return elf_path


def validate_elf(
    elf_path: Path,
    isram_limit: int = 16384,
    dsram_limit: int = 65536,
    stack_size: int = 4096,
    size_tool: str = "riscv32-unknown-elf-size",
) -> dict:
    """Post-link ELF size validation.

    Returns dict with section sizes.
    Raises ValueError if constraints are violated.
    """
    result = subprocess.run(
        [size_tool, str(elf_path)],
        capture_output=True, text=True, check=True,
    )

    lines = result.stdout.strip().split("\n")
    if len(lines) < 2:
        raise ValueError(f"Unexpected size output:\n{result.stdout}")

    # Parse: "   text    data     bss     dec     hex filename"
    parts = lines[1].split()
    text_size = int(parts[0])
    data_size = int(parts[1])
    bss_size = int(parts[2])

    if text_size > isram_limit:
        raise ValueError(
            f"I-SRAM overflow: .text={text_size}B > {isram_limit}B"
        )

    data_total = data_size + bss_size
    data_limit = dsram_limit - stack_size
    if data_total > data_limit:
        raise ValueError(
            f"D-SRAM overflow: .data+.bss={data_total}B > "
            f"{data_limit}B (after {stack_size}B stack)"
        )

    return {
        "text": text_size,
        "data": data_size,
        "bss": bss_size,
        "total_dec": int(parts[3]),
    }
