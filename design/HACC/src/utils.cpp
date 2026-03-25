#include "utils.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstring>
#include <cstdio>

namespace hacc {



static const char* ph_type_to_string(uint32_t t) {
	switch (t) {
		case PT_NULL: return "PT_NULL";
		case PT_LOAD: return "PT_LOAD";
		case PT_DYNAMIC: return "PT_DYNAMIC";
		case PT_INTERP: return "PT_INTERP";
		case PT_NOTE: return "PT_NOTE";
		case PT_SHLIB: return "PT_SHLIB";
		case PT_PHDR: return "PT_PHDR";
		case PT_TLS: return "PT_TLS";
		default: return "PT_UNKNOWN";
	}
}

static const char* sh_type_to_string(uint32_t t) {
	switch (t) {
		case SHT_NULL: return "SHT_NULL";
		case SHT_PROGBITS: return "SHT_PROGBITS";
		case SHT_SYMTAB: return "SHT_SYMTAB";
		case SHT_STRTAB: return "SHT_STRTAB";
		case SHT_RELA: return "SHT_RELA";
		case SHT_HASH: return "SHT_HASH";
		case SHT_DYNAMIC: return "SHT_DYNAMIC";
		case SHT_NOTE: return "SHT_NOTE";
		case SHT_NOBITS: return "SHT_NOBITS";
		case SHT_REL: return "SHT_REL";
		case SHT_SHLIB: return "SHT_SHLIB";
		case SHT_DYNSYM: return "SHT_DYNSYM";
		default: return "SHT_UNKNOWN";
	}
}

static const char* sh_flags_to_string(uint64_t f) {
    static char buf[64];
    buf[0] = '\0';
    if (f & SHF_WRITE) std::strcat(buf, "W");
    if (f & SHF_ALLOC) std::strcat(buf, "A");
    if (f & SHF_EXECINSTR) std::strcat(buf, "X");
    return buf;
}

static const char* ph_flags_to_string(uint32_t f) {
    static char buf[64];
    buf[0] = '\0';
    if (f & PF_R) std::strcat(buf, "R");
    if (f & PF_W) std::strcat(buf, "W");
    if (f & PF_X) std::strcat(buf, "X");
    return buf;
}

static const char* e_type_to_string(uint16_t t) {
    switch (t) {
        case ET_NONE: return "ET_NONE";
        case ET_REL: return "ET_REL";
        case ET_EXEC: return "ET_EXEC";
        case ET_DYN: return "ET_DYN";
        case ET_CORE: return "ET_CORE";
        default: return "ET_UNKNOWN";
    }
}

static const char* e_machine_to_string(uint16_t m) {
    switch (m) {
        case EM_NONE: return "EM_NONE";
        case EM_386: return "EM_386";
        case EM_X86_64: return "EM_X86_64";
        case EM_ARM: return "EM_ARM";
        case EM_AARCH64: return "EM_AARCH64";
        case EM_RISCV: return "EM_RISCV";
        default: return "EM_UNKNOWN";
    }
}

static const char* reg_name(uint8_t r) {
	static const char* names[] = {
		"r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
		"r8",  "r9",  "r10", "r11", "r12", "sp",  "lr",  "r15"
	};
	return (r < 16) ? names[r] : "r?";
}

std::string disasm_insn(uint32_t w, uint32_t pc) {
	uint8_t  opc   = (w >> 26) & 0x3F;
	uint8_t  rd    = (w >> 22) & 0xF;
	uint8_t  rs1   = (w >> 18) & 0xF;
	uint8_t  rs2   = (w >> 14) & 0xF;
	uint32_t imm18 = w & 0x3FFFF;
	uint32_t imm26 = w & 0x3FFFFFF;
	int32_t  simm18 = (imm18 & 0x20000) ? (int32_t)(imm18 | 0xFFFC0000) : (int32_t)imm18;
	int32_t  simm26 = (imm26 & 0x2000000) ? (int32_t)(imm26 | 0xFC000000) : (int32_t)imm26;

	char buf[128];

	switch (opc) {
	/* Format-I: immediate */
	case 0x01: std::snprintf(buf, sizeof(buf), "movi    %s, %d",            reg_name(rd), simm18); break;
	case 0x02: std::snprintf(buf, sizeof(buf), "movhi   %s, 0x%x",          reg_name(rd), imm18); break;
	case 0x05: std::snprintf(buf, sizeof(buf), "addi    %s, %s, %d",        reg_name(rd), reg_name(rs1), simm18); break;
	case 0x0D: std::snprintf(buf, sizeof(buf), "cmpi    %s, %d",            reg_name(rs1), simm18); break;

	/* Format-R: register */
	case 0x03: std::snprintf(buf, sizeof(buf), "mov     %s, %s",            reg_name(rd), reg_name(rs1)); break;
	case 0x04: std::snprintf(buf, sizeof(buf), "add     %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x06: std::snprintf(buf, sizeof(buf), "sub     %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x07: std::snprintf(buf, sizeof(buf), "and     %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x08: std::snprintf(buf, sizeof(buf), "or      %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x09: std::snprintf(buf, sizeof(buf), "xor     %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x0A: std::snprintf(buf, sizeof(buf), "shl     %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x0B: std::snprintf(buf, sizeof(buf), "shr     %s, %s, %s",        reg_name(rd), reg_name(rs1), reg_name(rs2)); break;
	case 0x0C: std::snprintf(buf, sizeof(buf), "cmp     %s, %s",            reg_name(rs1), reg_name(rs2)); break;

	/* Format-J: branch / call */
	case 0x10: std::snprintf(buf, sizeof(buf), "b       0x%08x",            pc + (uint32_t)(simm26 << 2)); break;
	case 0x11: std::snprintf(buf, sizeof(buf), "beq     0x%08x",            pc + (uint32_t)(simm26 << 2)); break;
	case 0x12: std::snprintf(buf, sizeof(buf), "bne     0x%08x",            pc + (uint32_t)(simm26 << 2)); break;
	case 0x13: std::snprintf(buf, sizeof(buf), "blt     0x%08x",            pc + (uint32_t)(simm26 << 2)); break;
	case 0x14: std::snprintf(buf, sizeof(buf), "bge     0x%08x",            pc + (uint32_t)(simm26 << 2)); break;
	case 0x15: std::snprintf(buf, sizeof(buf), "call    0x%08x",            pc + (uint32_t)(simm26 << 2)); break;
	case 0x16: std::snprintf(buf, sizeof(buf), "callr   %s",                reg_name(rs1)); break;
	case 0x17: std::snprintf(buf, sizeof(buf), "ret"); break;

	/* No operation / halt */
	case 0x18: std::snprintf(buf, sizeof(buf), "nop"); break;
	case 0x19: std::snprintf(buf, sizeof(buf), "hlt"); break;

	/* Memory */
	case 0x20: std::snprintf(buf, sizeof(buf), "ldw     %s, [%s, %d]",      reg_name(rd), reg_name(rs1), simm18); break;
	case 0x21: std::snprintf(buf, sizeof(buf), "stw     %s, [%s, %d]",      reg_name(rd), reg_name(rs1), simm18); break;
	case 0x22: std::snprintf(buf, sizeof(buf), "ldb     %s, [%s, %d]",      reg_name(rd), reg_name(rs1), simm18); break;
	case 0x23: std::snprintf(buf, sizeof(buf), "stb     %s, [%s, %d]",      reg_name(rd), reg_name(rs1), simm18); break;
	case 0x24: std::snprintf(buf, sizeof(buf), "push    %s",                reg_name(rd)); break;
	case 0x25: std::snprintf(buf, sizeof(buf), "pop     %s",                reg_name(rd)); break;

	/* CSR */
	case 0x28: std::snprintf(buf, sizeof(buf), "csrrd   %s, [%s, 0x%x]",   reg_name(rd), reg_name(rs1), imm18); break;
	case 0x29: std::snprintf(buf, sizeof(buf), "csrwr   %s, [%s, 0x%x]",   reg_name(rd), reg_name(rs1), imm18); break;
	case 0x2A: std::snprintf(buf, sizeof(buf), "csrsi   [%s, 0x%x]",       reg_name(rs1), imm18); break;
	case 0x2B: std::snprintf(buf, sizeof(buf), "csrcl   [%s, 0x%x]",       reg_name(rs1), imm18); break;

	/* MMIO */
	case 0x30: std::snprintf(buf, sizeof(buf), "mmiow   %s, [%s, %d]",     reg_name(rd), reg_name(rs1), simm18); break;
	case 0x31: std::snprintf(buf, sizeof(buf), "mmior   %s, [%s, %d]",     reg_name(rd), reg_name(rs1), simm18); break;
	case 0x32: std::snprintf(buf, sizeof(buf), "mmiowb  %s, [%s, %d]",     reg_name(rd), reg_name(rs1), simm18); break;
	case 0x33: std::snprintf(buf, sizeof(buf), "mmiord  %s, [%s, %d]",     reg_name(rd), reg_name(rs1), simm18); break;

	/* Stream */
	case 0x38: std::snprintf(buf, sizeof(buf), "strm    %s, [%s, %d]",     reg_name(rd), reg_name(rs1), simm18); break;
	case 0x39: std::snprintf(buf, sizeof(buf), "strmi   0x%x",             imm18); break;
	case 0x3A: std::snprintf(buf, sizeof(buf), "strmc   0x%x",             imm18); break;

	/* System */
	case 0x3C: std::snprintf(buf, sizeof(buf), "wfi"); break;
	case 0x3D: std::snprintf(buf, sizeof(buf), "wait    0x%x",             imm18); break;
	case 0x3E: std::snprintf(buf, sizeof(buf), "ackirq"); break;
	case 0x3F: {
		uint8_t subop = w & 0xFF;
		if (subop == 0x00) std::snprintf(buf, sizeof(buf), "ei");
		else if (subop == 0x01) std::snprintf(buf, sizeof(buf), "di");
		else if (subop == 0x02) std::snprintf(buf, sizeof(buf), "iret");
		else std::snprintf(buf, sizeof(buf), "sys     0x%06x", w & 0x3FFFFFF);
		break;
	}

	default:
		std::snprintf(buf, sizeof(buf), ".word   0x%08x", w);
		break;
	}

	return std::string(buf);
}

/// Print a hex dump of raw bytes, 16 bytes per line.
static void hex_dump(const std::vector<uint8_t> &data, uint64_t base_addr) {
	for (size_t off = 0; off < data.size(); off += 16) {
		std::printf("  %08x: ", (uint32_t)(base_addr + off));
		for (size_t j = 0; j < 16; ++j) {
			if (off + j < data.size())
				std::printf("%02x ", data[off + j]);
			else
				std::printf("   ");
			if (j == 7) std::printf(" ");
		}
		std::printf(" |");
		for (size_t j = 0; j < 16 && off + j < data.size(); ++j) {
			uint8_t c = data[off + j];
			std::printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
		}
		std::printf("|\n");
	}
}

/// Disassemble a section as 32-bit HACC MCU instructions.
static void disasm_section(const std::vector<uint8_t> &data, uint64_t base_addr) {
	for (size_t off = 0; off + 3 < data.size(); off += 4) {
		uint32_t w = (uint32_t)data[off]
		           | ((uint32_t)data[off+1] << 8)
		           | ((uint32_t)data[off+2] << 16)
		           | ((uint32_t)data[off+3] << 24);
		uint32_t pc = (uint32_t)(base_addr + off);
		std::printf("  %08x:  %08x    %s\n", pc, w, disasm_insn(w, pc).c_str());
	}
}

bool dump_elf(const std::string &path, const DumpOpts &opts) {
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs) {
		std::cerr << "failed to open: " << path << '\n';
		return false;
	}

	unsigned char ident[EI_NIDENT];
	ifs.read(reinterpret_cast<char*>(ident), EI_NIDENT);
	if (!ifs) {
		std::cerr << "failed to read ELF ident" << '\n';
		return false;
	}
	if (std::memcmp(ident, ELFMAG, SELFMAG) != 0) {
		std::cerr << "not an ELF file: " << path << '\n';
		return false;
	}

	if (ident[EI_CLASS] == ELFCLASS64) {
		Elf64_Ehdr eh;
		ifs.seekg(0);
		ifs.read(reinterpret_cast<char*>(&eh), sizeof(eh));
		if (!ifs) { std::cerr << "failed to read Elf64_Ehdr" << '\n'; return false; }

		std::cout << "ELF64 header:\n";
		std::cout << "  type: " << e_type_to_string(eh.e_type) << "  machine: " << e_machine_to_string(eh.e_machine) << "  entry: 0x" << std::hex << eh.e_entry << std::dec << '\n';
		std::cout << "  phoff: " << eh.e_phoff << "  shoff: " << eh.e_shoff << "\n";
		std::cout << "  phentsize: " << eh.e_phentsize << "  phnum: " << eh.e_phnum << "\n";
		std::cout << "  shentsize: " << eh.e_shentsize << "  shnum: " << eh.e_shnum << "  shstrndx: " << eh.e_shstrndx << "\n";

		// program headers
		if (eh.e_phoff != 0 && eh.e_phnum > 0) {
			ifs.seekg(eh.e_phoff);
			std::cout << "\nProgram headers (segments):\n";
			for (uint16_t i = 0; i < eh.e_phnum; ++i) {
				Elf64_Phdr ph;
				ifs.read(reinterpret_cast<char*>(&ph), sizeof(ph));
				if (!ifs) { std::cerr << "failed to read program header " << i << '\n'; return false; }
				std::cout << "  [" << i << "] type=" << ph_type_to_string(ph.p_type) << " flags=" << ph_flags_to_string(ph.p_flags)
						  << " offset=" << ph.p_offset << " vaddr=0x" << std::hex << ph.p_vaddr << std::dec
						  << " filesz=" << ph.p_filesz << " memsz=" << ph.p_memsz << "\n";
			}
		}

		// section headers
		std::vector<Elf64_Shdr> shdrs;
		if (eh.e_shoff != 0 && eh.e_shnum > 0) {
			shdrs.resize(eh.e_shnum);
			ifs.seekg(eh.e_shoff);
			for (uint16_t i = 0; i < eh.e_shnum; ++i) {
				ifs.read(reinterpret_cast<char*>(&shdrs[i]), sizeof(Elf64_Shdr));
				if (!ifs) { std::cerr << "failed to read section header " << i << '\n'; return false; }
			}

			// read section header string table
			std::string shstr;
			if (eh.e_shstrndx < shdrs.size()) {
				const Elf64_Shdr &s = shdrs[eh.e_shstrndx];
				if (s.sh_size > 0) {
					shstr.resize(s.sh_size);
					ifs.seekg(s.sh_offset);
					ifs.read(&shstr[0], s.sh_size);
				}
			}

			std::cout << "\nSection headers:\n";
			for (size_t i = 0; i < shdrs.size(); ++i) {
				const Elf64_Shdr &sh = shdrs[i];
				const char *name = "";
				if (!shstr.empty() && sh.sh_name < shstr.size()) name = &shstr[sh.sh_name];
				std::cout << "  [" << i << "] " << name << " type=" << sh_type_to_string(sh.sh_type)
						  << " flags=0x" << std::hex << sh.sh_flags << std::dec
						  << " addr=0x" << std::hex << sh.sh_addr << std::dec
						  << " offset=" << sh.sh_offset << " size=" << sh.sh_size << "\n";
			}

			// section content decode / disassembly
			if (opts.decode || opts.disasm) {
				for (size_t i = 0; i < shdrs.size(); ++i) {
					const Elf64_Shdr &sh = shdrs[i];
					if (sh.sh_type == SHT_NULL || sh.sh_size == 0) continue;
					const char *name = "";
					if (!shstr.empty() && sh.sh_name < shstr.size()) name = &shstr[sh.sh_name];

					std::vector<uint8_t> sec(sh.sh_size);
					if (sh.sh_type != SHT_NOBITS) {
						ifs.seekg(sh.sh_offset);
						ifs.read(reinterpret_cast<char*>(sec.data()), sh.sh_size);
						if (!ifs) { std::cerr << "failed to read section " << name << '\n'; continue; }
					}

					bool is_exec = (sh.sh_flags & SHF_EXECINSTR) != 0;
					if (opts.disasm && is_exec) {
						std::printf("\nDisassembly of section %s (%lu bytes):\n", name, (unsigned long)sh.sh_size);
						disasm_section(sec, sh.sh_addr);
					} else if (opts.decode) {
						std::printf("\nHex dump of section %s (%lu bytes):\n", name, (unsigned long)sh.sh_size);
						hex_dump(sec, sh.sh_addr);
					}
				}
			}
		}

	} else if (ident[EI_CLASS] == ELFCLASS32) {
		Elf32_Ehdr eh;
		ifs.seekg(0);
		ifs.read(reinterpret_cast<char*>(&eh), sizeof(eh));
		if (!ifs) { std::cerr << "failed to read Elf32_Ehdr" << '\n'; return false; }

		std::cout << "ELF32 header:\n";
		std::cout << "  type: " << e_type_to_string(eh.e_type) << "  machine: " << e_machine_to_string(eh.e_machine) << "  entry: 0x" << std::hex << eh.e_entry << std::dec << '\n';
		std::cout << "  phoff: " << eh.e_phoff << "  shoff: " << eh.e_shoff << "\n";
		std::cout << "  phentsize: " << eh.e_phentsize << "  phnum: " << eh.e_phnum << "\n";
		std::cout << "  shentsize: " << eh.e_shentsize << "  shnum: " << eh.e_shnum << "  shstrndx: " << eh.e_shstrndx << "\n";

		// program headers
		if (eh.e_phoff != 0 && eh.e_phnum > 0) {
			ifs.seekg(eh.e_phoff);
			std::cout << "\nProgram headers (segments):\n";
			for (uint16_t i = 0; i < eh.e_phnum; ++i) {
				Elf32_Phdr ph;
				ifs.read(reinterpret_cast<char*>(&ph), sizeof(ph));
				if (!ifs) { std::cerr << "failed to read program header " << i << '\n'; return false; }
				std::cout << "  [" << i << "] type=" << ph_type_to_string(ph.p_type) << " flags=" << ph_flags_to_string(ph.p_flags)
						  << " offset=" << ph.p_offset << " vaddr=0x" << std::hex << ph.p_vaddr << std::dec
						  << " filesz=" << ph.p_filesz << " memsz=" << ph.p_memsz << "\n";
			}
		}

		// section headers
		std::vector<Elf32_Shdr> shdrs;
		if (eh.e_shoff != 0 && eh.e_shnum > 0) {
			shdrs.resize(eh.e_shnum);
			ifs.seekg(eh.e_shoff);
			for (uint16_t i = 0; i < eh.e_shnum; ++i) {
				ifs.read(reinterpret_cast<char*>(&shdrs[i]), sizeof(Elf32_Shdr));
				if (!ifs) { std::cerr << "failed to read section header " << i << '\n'; return false; }
			}

			// read section header string table
			std::string shstr;
			if (eh.e_shstrndx < shdrs.size()) {
				const Elf32_Shdr &s = shdrs[eh.e_shstrndx];
				if (s.sh_size > 0) {
					shstr.resize(s.sh_size);
					ifs.seekg(s.sh_offset);
					ifs.read(&shstr[0], s.sh_size);
				}
			}

			std::cout << "\nSection headers:\n";
			for (size_t i = 0; i < shdrs.size(); ++i) {
				const Elf32_Shdr &sh = shdrs[i];
				const char *name = "";
				if (!shstr.empty() && sh.sh_name < shstr.size()) name = &shstr[sh.sh_name];
				std::cout << "  [" << i << "] " << name << " type=" << sh_type_to_string(sh.sh_type)
						  << " flags=0x" << std::hex << sh.sh_flags << std::dec
						  << " addr=0x" << std::hex << sh.sh_addr << std::dec
						  << " offset=" << sh.sh_offset << " size=" << sh.sh_size << "\n";
			}

			// section content decode / disassembly
			if (opts.decode || opts.disasm) {
				for (size_t i = 0; i < shdrs.size(); ++i) {
					const Elf32_Shdr &sh = shdrs[i];
					if (sh.sh_type == SHT_NULL || sh.sh_size == 0) continue;
					const char *name = "";
					if (!shstr.empty() && sh.sh_name < shstr.size()) name = &shstr[sh.sh_name];

					std::vector<uint8_t> sec(sh.sh_size);
					if (sh.sh_type != SHT_NOBITS) {
						ifs.seekg(sh.sh_offset);
						ifs.read(reinterpret_cast<char*>(sec.data()), sh.sh_size);
						if (!ifs) { std::cerr << "failed to read section " << name << '\n'; continue; }
					}

					bool is_exec = (sh.sh_flags & SHF_EXECINSTR) != 0;
					if (opts.disasm && is_exec) {
						std::printf("\nDisassembly of section %s (%u bytes):\n", name, sh.sh_size);
						disasm_section(sec, sh.sh_addr);
					} else if (opts.decode) {
						std::printf("\nHex dump of section %s (%u bytes):\n", name, sh.sh_size);
						hex_dump(sec, sh.sh_addr);
					}
				}
			}
		}

	} else {
		std::cerr << "unknown ELF class: " << static_cast<int>(ident[EI_CLASS]) << '\n';
		return false;
	}

	return true;
}

}
