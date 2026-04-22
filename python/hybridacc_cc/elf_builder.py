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


# Default ISA: RV32I + Zmmul (multiply-only, no DIV) + Zicsr
DEFAULT_MARCH = "rv32i_zmmul_zicsr"


def _normalize_opt_level(opt_level: str, *, leading_dash: bool) -> str:
    """Normalize optimization level strings like 3/O3/-O3."""
    normalized = opt_level.strip()
    if normalized.startswith("-O"):
        normalized = normalized[1:]
    elif not normalized.startswith("O"):
        normalized = f"O{normalized}"
    if leading_dash:
        return f"-{normalized}"
    return normalized


def build_gcc_command(
    output_dir: Path,
    elf_name: str = "firmware.elf",
    gcc: str = DEFAULT_GCC,
    march: str = DEFAULT_MARCH,
    mabi: str = "ilp32",
    opt_level: str = "-O2",
    mmio_opt_level: Optional[str] = None,
) -> List[str]:
    """Build the complete GCC cross-compilation command.

    Returns the command as a list of strings (suitable for subprocess).
    """
    sources = ["firmware_main.c", "firmware_data.c", "firmware_ops.c"]
    source_paths = [str(output_dir / s) for s in sources]
    linker_script = str(output_dir / "linker.ld")
    elf_path = str(output_dir / elf_name)
    normalized_opt_level = _normalize_opt_level(opt_level, leading_dash=True)

    cmd = [
        gcc,
        f"-march={march}",
        f"-mabi={mabi}",
        "-nostdlib",
        "-ffreestanding",
        normalized_opt_level,
        "-Wall", "-Wextra",
        "-Wl,--gc-sections",
        "-ffunction-sections", "-fdata-sections",
        f"-T{linker_script}",
        f"-I{output_dir}",
        "-o", elf_path,
    ]

    if mmio_opt_level is not None:
        normalized_mmio_opt = _normalize_opt_level(mmio_opt_level, leading_dash=False)
        cmd.append(f'-DHACC_MMIO_OPTIMIZE_LEVEL="{normalized_mmio_opt}"')

    cmd += source_paths

    return cmd


def compile_firmware(
    output_dir: Path,
    elf_name: str = "firmware.elf",
    gcc: str = DEFAULT_GCC,
    march: str = DEFAULT_MARCH,
    opt_level: str = "-O2",
    mmio_opt_level: Optional[str] = None,
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
    cmd = build_gcc_command(
        output_dir,
        elf_name,
        gcc,
        march=march,
        opt_level=opt_level,
        mmio_opt_level=mmio_opt_level,
    )
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
