/**
 * @file elf_packager.cpp
 * @brief HACC-ELF Packager implementation.
 *
 * Produces a minimal 32-bit little-endian ELF binary with custom
 * .hacc.* section names. Uses standard ELF headers (elf.h).
 */
#include "hacc/elf_packager.hpp"
#include <elf.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <numeric>

namespace hacc {

/**
 * @brief Internal section descriptor for ELF emission.
 */
struct SectionInfo {
    const char    *name;  ///< Section name string.
    const uint32_t *data; ///< Pointer to word data.
    uint32_t       size;  ///< Size in bytes.
    uint32_t       flags; ///< ELF section flags.
    uint32_t       addr;  ///< Load address (VMA).
};

bool ElfPackager::write_elf(const HaccPackage &pkg, const std::string &path) {
    /*
     * Section table:
     *   [0] null
     *   [1] .hacc.core
     *   [2] .hacc.job
     *   [3] .hacc.block
     *   [4] .hacc.profile
     *   [5] .hacc.dma
     *   [6] .hacc.agu
     *   [7] .hacc.nlu
     *   [8] .hacc.pe
     *   [9] .hacc.scan
     *   [10] .hacc.patch
     *   [11] .shstrtab
     */
    constexpr int NUM_SECTIONS = 12;

    SectionInfo sections[] = {
        {".hacc.core",    pkg.core_firmware.data(),
         static_cast<uint32_t>(pkg.core_firmware.size() * 4), SHF_ALLOC | SHF_EXECINSTR, 0x00000000},
        {".hacc.job",     pkg.job_section.data(),
         static_cast<uint32_t>(pkg.job_section.size() * 4),   SHF_ALLOC, 0x10000000},
        {".hacc.block",   pkg.block_section.data(),
         static_cast<uint32_t>(pkg.block_section.size() * 4), SHF_ALLOC, 0},
        {".hacc.profile", pkg.profile_section.data(),
         static_cast<uint32_t>(pkg.profile_section.size() * 4), SHF_ALLOC, 0},
        {".hacc.dma",     pkg.dma_section.data(),
         static_cast<uint32_t>(pkg.dma_section.size() * 4),  SHF_ALLOC, 0},
        {".hacc.agu",     pkg.agu_section.data(),
         static_cast<uint32_t>(pkg.agu_section.size() * 4),  SHF_ALLOC, 0},
        {".hacc.nlu",     pkg.nlu_section.data(),
         static_cast<uint32_t>(pkg.nlu_section.size() * 4),  SHF_ALLOC, 0},
        {".hacc.pe",      pkg.pe_section.data(),
         static_cast<uint32_t>(pkg.pe_section.size() * 4),   SHF_ALLOC, 0},
        {".hacc.scan",    pkg.scan_section.data(),
         static_cast<uint32_t>(pkg.scan_section.size() * 4), SHF_ALLOC, 0},
        {".hacc.patch",   pkg.patch_section.data(),
         static_cast<uint32_t>(pkg.patch_section.size() * 4), SHF_ALLOC, 0},
    };
    constexpr int DATA_SECTIONS = 10;

    /* Build shstrtab. */
    std::string shstrtab;
    shstrtab.push_back('\0'); /* null entry name at offset 0 */
    uint32_t name_offsets[DATA_SECTIONS];
    for (int i = 0; i < DATA_SECTIONS; ++i) {
        name_offsets[i] = static_cast<uint32_t>(shstrtab.size());
        shstrtab.append(sections[i].name);
        shstrtab.push_back('\0');
    }
    uint32_t shstrtab_name_off = static_cast<uint32_t>(shstrtab.size());
    shstrtab.append(".shstrtab");
    shstrtab.push_back('\0');

    /* Compute file layout. */
    uint32_t offset = sizeof(Elf32_Ehdr);

    /* Section data follows ELF header. */
    uint32_t section_offsets[DATA_SECTIONS];
    for (int i = 0; i < DATA_SECTIONS; ++i) {
        /* Align to 4 bytes. */
        offset = (offset + 3) & ~3u;
        section_offsets[i] = offset;
        offset += sections[i].size;
    }

    /* shstrtab data. */
    uint32_t shstrtab_offset = (offset + 3) & ~3u;
    offset = shstrtab_offset + static_cast<uint32_t>(shstrtab.size());

    /* Section header table — align to 4. */
    uint32_t shdr_offset = (offset + 3) & ~3u;

    /* Build ELF header. */
    Elf32_Ehdr ehdr;
    std::memset(&ehdr, 0, sizeof(ehdr));
    std::memcpy(ehdr.e_ident, ELFMAG, SELFMAG);
    ehdr.e_ident[EI_CLASS]   = ELFCLASS32;
    ehdr.e_ident[EI_DATA]    = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_NONE; /* custom target */
    ehdr.e_version   = EV_CURRENT;
    ehdr.e_entry     = 0;
    ehdr.e_phoff     = 0;
    ehdr.e_shoff     = shdr_offset;
    ehdr.e_flags     = 0;
    ehdr.e_ehsize    = sizeof(Elf32_Ehdr);
    ehdr.e_phentsize = 0;
    ehdr.e_phnum     = 0;
    ehdr.e_shentsize = sizeof(Elf32_Shdr);
    ehdr.e_shnum     = NUM_SECTIONS;
    ehdr.e_shstrndx  = NUM_SECTIONS - 1; /* last section = shstrtab */

    /* Build section headers. */
    Elf32_Shdr shdrs[NUM_SECTIONS];
    std::memset(shdrs, 0, sizeof(shdrs));

    /* [0] null section. */

    /* [1..10] data sections. */
    for (int i = 0; i < DATA_SECTIONS; ++i) {
        shdrs[i + 1].sh_name    = name_offsets[i];
        shdrs[i + 1].sh_type    = SHT_PROGBITS;
        shdrs[i + 1].sh_flags   = sections[i].flags;
        shdrs[i + 1].sh_addr    = sections[i].addr;
        shdrs[i + 1].sh_offset  = section_offsets[i];
        shdrs[i + 1].sh_size    = sections[i].size;
        shdrs[i + 1].sh_addralign = 4;
    }

    /* [11] shstrtab. */
    shdrs[NUM_SECTIONS - 1].sh_name      = shstrtab_name_off;
    shdrs[NUM_SECTIONS - 1].sh_type      = SHT_STRTAB;
    shdrs[NUM_SECTIONS - 1].sh_offset    = shstrtab_offset;
    shdrs[NUM_SECTIONS - 1].sh_size      = static_cast<uint32_t>(shstrtab.size());
    shdrs[NUM_SECTIONS - 1].sh_addralign = 1;

    /* Write file. */
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;

    ofs.write(reinterpret_cast<const char*>(&ehdr), sizeof(ehdr));

    /* Write section data with padding. */
    for (int i = 0; i < DATA_SECTIONS; ++i) {
        /* Pad to section offset. */
        auto cur = static_cast<uint32_t>(ofs.tellp());
        while (cur < section_offsets[i]) {
            char z = 0;
            ofs.write(&z, 1);
            ++cur;
        }
        if (sections[i].size > 0) {
            ofs.write(reinterpret_cast<const char*>(sections[i].data), sections[i].size);
        }
    }

    /* Pad to shstrtab offset. */
    {
        auto cur = static_cast<uint32_t>(ofs.tellp());
        while (cur < shstrtab_offset) {
            char z = 0;
            ofs.write(&z, 1);
            ++cur;
        }
    }
    ofs.write(shstrtab.data(), shstrtab.size());

    /* Pad to shdr offset. */
    {
        auto cur = static_cast<uint32_t>(ofs.tellp());
        while (cur < shdr_offset) {
            char z = 0;
            ofs.write(&z, 1);
            ++cur;
        }
    }
    ofs.write(reinterpret_cast<const char*>(shdrs), sizeof(shdrs));

    return ofs.good();
}

std::string ElfPackager::generate_debug_json(const HaccPackage &pkg, const CompilerContext &ctx) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"stage\": \"stage0\",\n";
    os << "  \"op_name\": \"" << ctx.op.name << "\",\n";
    os << "  \"op_type\": " << static_cast<int>(ctx.op.op_type) << ",\n";

    /* Input/output shapes. */
    os << "  \"input_shape\": [" << ctx.op.input.n << "," << ctx.op.input.c
       << "," << ctx.op.input.h << "," << ctx.op.input.w << "],\n";
    os << "  \"output_shape\": [" << ctx.op.output.n << "," << ctx.op.output.c
       << "," << ctx.op.output.h << "," << ctx.op.output.w << "],\n";

    /* Schedule. */
    os << "  \"loop_extents\": [";
    for (int i = 0; i < 4; ++i) {
        if (i) os << ",";
        os << ctx.schedule.loop_extents[i];
    }
    os << "],\n";
    os << "  \"loop_names\": [";
    for (int i = 0; i < 4; ++i) {
        if (i) os << ",";
        os << "\"" << ctx.schedule.loop_names[i] << "\"";
    }
    os << "],\n";
    os << "  \"total_waves\": " << ctx.schedule.total_waves() << ",\n";
    os << "  \"num_clusters\": " << ctx.schedule.num_clusters << ",\n";
    os << "  \"cluster_mask\": " << ctx.schedule.cluster_mask << ",\n";

    /* Section sizes. */
    os << "  \"sections\": {\n";
    os << "    \"core_words\": " << pkg.core_firmware.size() << ",\n";
    os << "    \"job_words\": " << pkg.job_section.size() << ",\n";
    os << "    \"block_words\": " << pkg.block_section.size() << ",\n";
    os << "    \"profile_words\": " << pkg.profile_section.size() << ",\n";
    os << "    \"dma_words\": " << pkg.dma_section.size() << ",\n";
    os << "    \"agu_words\": " << pkg.agu_section.size() << ",\n";
    os << "    \"nlu_words\": " << pkg.nlu_section.size() << ",\n";
    os << "    \"pe_words\": " << pkg.pe_section.size() << ",\n";
    os << "    \"scan_words\": " << pkg.scan_section.size() << ",\n";
    os << "    \"patch_words\": " << pkg.patch_section.size() << "\n";
    os << "  },\n";

    /* Block summary. */
    os << "  \"blocks\": [\n";
    for (size_t i = 0; i < ctx.blocks.size(); ++i) {
        const auto &b = ctx.blocks[i];
        os << "    {\"loop_rank\":" << b.loop_rank
           << ",\"loop_extent\":[" << b.loop_extent[0] << "," << b.loop_extent[1]
           << "," << b.loop_extent[2] << "," << b.loop_extent[3] << "]"
           << ",\"cluster_mask\":" << b.cluster_mask
           << ",\"rule_stride\":" << b.rule_stride
           << ",\"total_waves\":" << b.total_waves
           << ",\"patch_count\":" << b.patch_count
           << ",\"flags\":" << b.block_flags
           << "}";
        if (i + 1 < ctx.blocks.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    /* Patch summary. */
    os << "  \"patch_count\": " << ctx.patches.size() << ",\n";
    os << "  \"nlu_rule_count\": " << ctx.nlu_rules.size() << "\n";

    os << "}\n";
    return os.str();
}

} // namespace hacc
