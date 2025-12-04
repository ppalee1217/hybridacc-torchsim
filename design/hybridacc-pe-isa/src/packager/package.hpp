// Package binary format definitions for HybridAcc PE-ISA
// Creates package binaries containing multiple templates
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include "../assambler/instruction.hpp"

namespace hybridacc {

// Single template in a package
struct PackageTemplate {
    std::string name;
    std::vector<TemplateParam> params;
    std::vector<TemplatePatch> patches;
    std::vector<uint16_t> instructions;

    // Source file info
    std::string sourcePath;

    PackageTemplate() : name(""), sourcePath("") {}
};

// Package format:
// Header:
//   [package version, 8bits]
//   [num of templates, 8bits]
//   [offset vector 0, 16bits]
//   [offset vector 1, 16bits]
//   ...
// Body:
//   [template binary 0]
//   [template binary 1]
//   ...
struct Package {
    uint8_t version;
    std::vector<PackageTemplate> templates;

    Package() : version(1) {}

    // Calculate total binary size
    size_t calculateBinarySize() const;

    // Calculate offset for each template in the package binary
    std::vector<uint16_t> calculateOffsets() const;
};

class Packager {
public:
    Packager();

    // Add template from .asm file (needs to call assembler first)
    void addTemplateFromAsm(const std::string &asmPath);

    // Add template from .json file (already assembled)
    void addTemplateFromJson(const std::string &jsonPath);

    // Create package
    Package createPackage() const;

    // Write outputs
    void writePackageBinary(const std::string &path, const Package &pkg) const;
    void writePackageJson(const std::string &path, const Package &pkg) const;
    void writePackageHeader(const std::string &path, const Package &pkg) const;

private:
    std::vector<PackageTemplate> templates_;

    // Helper: load template from JSON
    PackageTemplate loadFromJson(const std::string &jsonPath) const;

    // Helper: assemble and load template from ASM
    PackageTemplate loadFromAsm(const std::string &asmPath) const;
};

} // namespace hybridacc
