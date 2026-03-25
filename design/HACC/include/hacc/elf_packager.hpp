/**
 * @file elf_packager.hpp
 * @brief HACC-ELF Packager.
 *
 * Packages the compiled HaccPackage into a minimal ELF binary with
 * custom .hacc.* sections. The output can be loaded by the HACC
 * section loader into local memories.
 *
 * @see HACC.md §4 Package & Section Contract
 * @see HACC.md §8 Compiler output & review artifact
 */
#pragma once
#include "hacc/ir.hpp"
#include <string>

namespace hacc {

/**
 * @brief HACC-ELF packager.
 *
 * Writes a minimal 32-bit little-endian ELF containing all .hacc.* sections,
 * plus generates a Stage0 debug JSON for human review.
 */
class ElfPackager {
public:
    /**
     * @brief Write the HACC-ELF binary to a file.
     *
     * @param pkg   Compiled package containing all sections.
     * @param path  Output file path for the ELF binary.
     * @return true on success, false on I/O failure.
     */
    static bool write_elf(const HaccPackage &pkg, const std::string &path);

    /**
     * @brief Generate a Stage0 debug JSON string.
     *
     * The JSON includes job summary, block list, loop extents, table sizes,
     * and payload checksums for human review.
     *
     * @param pkg  Compiled package.
     * @param ctx  Compiler context for metadata.
     * @return JSON string.
     *
     * @see HACC.md §8.2 Stage0 required information
     */
    static std::string generate_debug_json(const HaccPackage &pkg, const CompilerContext &ctx);
};

} // namespace hacc
