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


def _derive_binutil_tool(gcc: str, tool_name: str) -> str:
    """Derive binutils companion name from the configured GCC executable."""
    gcc_path = Path(gcc)
    gcc_name = gcc_path.name
    if gcc_name.endswith("gcc"):
        return str(gcc_path.with_name(f"{gcc_name[:-3]}{tool_name}"))
    return tool_name


def audit_zero_init_sections(
    elf_path: Path,
    readelf_tool: str = "riscv32-unknown-elf-readelf",
) -> None:
    """Fail if writable small-data zero-init sections escaped the cleared .bss region."""
    try:
        result = subprocess.run(
            [readelf_tool, "-W", "-S", str(elf_path)],
            capture_output=True, text=True, check=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"ELF zero-init audit requires '{readelf_tool}' in PATH"
        ) from exc

    leaked_sections: list[tuple[str, int]] = []
    for line in result.stdout.splitlines():
        match = re.match(
            r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+\S+\s+\S+\s+([0-9A-Fa-f]+)\s+\S+\s+(\S+)",
            line,
        )
        if not match:
            continue

        name = match.group(1)
        size = int(match.group(2), 16)
        flags = match.group(3)

        if size == 0:
            continue
        if "W" not in flags or "A" not in flags:
            continue
        if name.startswith(".sbss") or name == ".scommon" or name.startswith(".gnu.linkonce.sb."):
            leaked_sections.append((name, size))

    if leaked_sections:
        formatted = ", ".join(
            f"{name}={size:#x}" for name, size in leaked_sections
        )
        raise ValueError(
            "ELF zero-init audit failed: found writable small-data zero-init "
            f"sections outside consolidated .bss: {formatted}. "
            "Merge .sbss/.scommon into the linker-cleared zero-init region."
        )


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

    audit_zero_init_sections(
        elf_path,
        readelf_tool=_derive_binutil_tool(gcc, "readelf"),
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
