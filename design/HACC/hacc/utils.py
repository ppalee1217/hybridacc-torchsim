from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import io
import struct
import sys


@dataclass(slots=True)
class DumpOpts:
    header: bool = True
    decode: bool = False
    disasm: bool = False


REG_NAMES = ["r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "r15"]


def disasm_insn(word: int, pc: int) -> str:
    opc = (word >> 26) & 0x3F
    rd = (word >> 22) & 0xF
    rs1 = (word >> 18) & 0xF
    rs2 = (word >> 14) & 0xF
    imm18 = word & 0x3FFFF
    imm26 = word & 0x03FFFFFF
    simm18 = imm18 | (~0x3FFFF if imm18 & 0x20000 else 0)
    simm26 = imm26 | (~0x03FFFFFF if imm26 & 0x2000000 else 0)

    if opc == 0x01:
        return f"movi    {REG_NAMES[rd]}, {simm18}"
    if opc == 0x02:
        return f"movhi   {REG_NAMES[rd]}, 0x{imm18:x}"
    if opc == 0x04:
        return f"add     {REG_NAMES[rd]}, {REG_NAMES[rs1]}, {REG_NAMES[rs2]}"
    if opc == 0x05:
        return f"addi    {REG_NAMES[rd]}, {REG_NAMES[rs1]}, {simm18}"
    if opc == 0x0A:
        return f"shl     {REG_NAMES[rd]}, {REG_NAMES[rs1]}, {imm18}"
    if opc == 0x0C:
        return f"cmp     {REG_NAMES[rs1]}, {REG_NAMES[rs2]}"
    if opc == 0x10:
        return f"b       0x{(pc + (simm26 << 2)) & 0xFFFFFFFF:08x}"
    if opc == 0x14:
        return f"bge     0x{(pc + (simm18 << 2)) & 0xFFFFFFFF:08x}"
    if opc == 0x19:
        return "hlt"
    if opc == 0x20:
        return f"ldw     {REG_NAMES[rd]}, [{REG_NAMES[rs1]}, {simm18}]"
    if opc == 0x30:
        return f"mmiow   {REG_NAMES[rd]}, [{REG_NAMES[rs1]}, {simm18}]"
    if opc == 0x32:
        return f"mmiowb  {REG_NAMES[rd]}, [{REG_NAMES[rs1]}, {REG_NAMES[rs2]}]"
    if opc == 0x38:
        return f"strm    dst={rd}, {REG_NAMES[rs1]}, {REG_NAMES[rs2]}"
    if opc == 0x3C:
        return "wfi"
    if opc == 0x3E:
        return "ackirq"
    if opc == 0x3F:
        subop = word & 0xFF
        if subop == 0:
            return "ei"
        if subop == 1:
            return "di"
        if subop == 2:
            return "iret"
    return f".word   0x{word:08x}"


def _hex_dump(data: bytes, base_addr: int) -> str:
    lines: list[str] = []
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        hex_part = " ".join(f"{byte:02x}" for byte in chunk)
        ascii_part = "".join(chr(byte) if 32 <= byte < 127 else "." for byte in chunk)
        lines.append(f"  {base_addr + offset:08x}: {hex_part:<47} |{ascii_part}|")
    return "\n".join(lines)


def dump_elf(path: str, opts: DumpOpts = DumpOpts(), stream: io.TextIOBase | None = None) -> bool:
    output = stream or sys.stdout
    file_path = Path(path)
    data = file_path.read_bytes()
    if data[:4] != b"\x7fELF":
        output.write(f"not an ELF file: {path}\n")
        return False

    elf_class = data[4]
    if elf_class == 1:
        ehdr = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
        shoff = ehdr[6]
        shentsize = ehdr[11]
        shnum = ehdr[12]
        shstrndx = ehdr[13]
        if opts.header:
            output.write("ELF32 header:\n")
            output.write(f"  shoff: {shoff}  shnum: {shnum}  shstrndx: {shstrndx}\n")
        shdrs = [struct.unpack_from("<IIIIIIIIII", data, shoff + index * shentsize) for index in range(shnum)]
    else:
        ehdr = struct.unpack_from("<16sHHIQQQIHHHHHH", data, 0)
        shoff = ehdr[6]
        shentsize = ehdr[11]
        shnum = ehdr[12]
        shstrndx = ehdr[13]
        if opts.header:
            output.write("ELF64 header:\n")
            output.write(f"  shoff: {shoff}  shnum: {shnum}  shstrndx: {shstrndx}\n")
        shdrs = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * shentsize) for index in range(shnum)]

    shstr = b""
    if shstrndx < len(shdrs):
        sh = shdrs[shstrndx]
        sh_offset = sh[4] if elf_class == 2 else sh[4]
        sh_size = sh[5] if elf_class == 2 else sh[5]
        shstr = data[sh_offset : sh_offset + sh_size]

    output.write("\nSection headers:\n")
    for index, sh in enumerate(shdrs):
        sh_name = sh[0]
        sh_offset = sh[4] if elf_class == 2 else sh[4]
        sh_size = sh[5] if elf_class == 2 else sh[5]
        name = b""
        if sh_name < len(shstr):
            name = shstr[sh_name:].split(b"\x00", 1)[0]
        decoded_name = name.decode("ascii", errors="replace")
        output.write(f"  [{index}] {decoded_name} offset={sh_offset} size={sh_size}\n")
        if sh_size and (opts.decode or (opts.disasm and decoded_name == ".hacc.core")):
            section_data = data[sh_offset : sh_offset + sh_size]
            if opts.decode:
                output.write(_hex_dump(section_data, 0))
                output.write("\n")
            if opts.disasm and decoded_name == ".hacc.core":
                for pc_offset in range(0, len(section_data), 4):
                    word = int.from_bytes(section_data[pc_offset : pc_offset + 4], "little")
                    output.write(f"  {pc_offset:08x}: {word:08x}    {disasm_insn(word, pc_offset)}\n")
    return True