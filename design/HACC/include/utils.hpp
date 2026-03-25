#pragma once
#include <elf.h>
#include <string>
#include <cstdint>

namespace hacc {

/// Options controlling what elfdump displays.
struct DumpOpts {
	bool header   = true;   ///< Always show ELF/program/section headers.
	bool decode   = false;  ///< -x  Hex-dump section contents.
	bool disasm   = false;  ///< -d  Disassemble .hacc.core (HACC MCU ISA).
};

/// Returns true on success, false on failure.
bool dump_elf(const std::string &path, const DumpOpts &opts = {});

/// Disassemble a single 32-bit HACC MCU instruction word.
/// @param word  The 32-bit instruction word.
/// @param pc    The program counter (byte address) of this instruction.
/// @return Human-readable mnemonic string.
std::string disasm_insn(uint32_t word, uint32_t pc);

}