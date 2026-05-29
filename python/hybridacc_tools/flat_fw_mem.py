from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


SECTION_RE = re.compile(
    r"^\s*\d+\s+(\S+)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+\S+"
)


@dataclass(frozen=True)
class ElfSection:
    name: str
    size: int
    vma: int
    lma: int
    file_off: int
    flags: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Flatten HybridAcc firmware ELF sections into the RTL SectionLoader image layout."
    )
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--objcopy", default="riscv32-unknown-elf-objcopy")
    parser.add_argument("--objdump", default="riscv32-unknown-elf-objdump")
    parser.add_argument("--isram-bytes", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--dsram-base", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--dsram-bytes", required=True, type=lambda value: int(value, 0))
    return parser.parse_args()


def run_tool(command: list[str]) -> str:
    result = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def parse_sections(objdump_text: str) -> list[ElfSection]:
    sections: list[ElfSection] = []
    lines = objdump_text.splitlines()
    line_idx = 0

    while line_idx < len(lines):
        match = SECTION_RE.match(lines[line_idx])
        if not match:
            line_idx += 1
            continue

        flags = ""
        if line_idx + 1 < len(lines):
            flags = lines[line_idx + 1].strip()

        sections.append(
            ElfSection(
                name=match.group(1),
                size=int(match.group(2), 16),
                vma=int(match.group(3), 16),
                lma=int(match.group(4), 16),
                file_off=int(match.group(5), 16),
                flags=flags,
            )
        )
        line_idx += 2

    return sections


def map_vma_to_flat_offset(section: ElfSection, isram_bytes: int, dsram_base: int, dsram_bytes: int) -> int:
    if 0 <= section.vma < isram_bytes:
        return section.vma

    if dsram_base <= section.vma < (dsram_base + dsram_bytes):
        return isram_bytes + (section.vma - dsram_base)

    raise ValueError(
        f"Section {section.name} at VMA 0x{section.vma:08X} is outside the flat RTL loader map"
    )


def is_loadable_contents(section: ElfSection) -> bool:
    return section.size > 0 and "LOAD" in section.flags and "CONTENTS" in section.flags


def dump_section(objcopy: str, elf_path: Path, section: ElfSection, out_path: Path) -> bytes:
    subprocess.run(
        [objcopy, f"--dump-section", f"{section.name}={out_path}", str(elf_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    return out_path.read_bytes()


def ensure_size(image: bytearray, size: int) -> None:
    if len(image) < size:
        image.extend(b"\x00" * (size - len(image)))


def write_mem(output_path: Path, image: bytes) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="ascii") as output_file:
        for byte_idx in range(0, len(image), 16):
            chunk = image[byte_idx : byte_idx + 16]
            output_file.write(" ".join(f"{byte:02X}" for byte in chunk))
            output_file.write("\n")


def main() -> int:
    args = parse_args()

    try:
        objdump_text = run_tool([args.objdump, "-h", str(args.elf)])
        sections = [section for section in parse_sections(objdump_text) if is_loadable_contents(section)]

        if not sections:
            raise ValueError(f"No loadable sections found in {args.elf}")

        image = bytearray()
        with tempfile.TemporaryDirectory() as temp_dir_name:
            temp_dir = Path(temp_dir_name)
            for section_idx, section in enumerate(sections):
                flat_offset = map_vma_to_flat_offset(
                    section,
                    args.isram_bytes,
                    args.dsram_base,
                    args.dsram_bytes,
                )
                section_bin_path = temp_dir / f"section_{section_idx}.bin"
                section_data = dump_section(args.objcopy, args.elf, section, section_bin_path)

                if len(section_data) != section.size:
                    raise ValueError(
                        f"Section {section.name} size mismatch: objdump={section.size} dump={len(section_data)}"
                    )

                end_offset = flat_offset + len(section_data)
                ensure_size(image, end_offset)
                image[flat_offset:end_offset] = section_data

        if len(image) % 4 != 0:
            ensure_size(image, (len(image) + 3) & ~0x3)

        write_mem(args.output, image)
        print(len(image))
        return 0
    except Exception as exc:  # pragma: no cover - CLI path
        print(f"hacc-flat-fw-mem: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())