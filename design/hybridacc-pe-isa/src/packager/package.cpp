// Package implementation for HybridAcc PE-ISA
#include "package.hpp"
#include "../assambler/utils.hpp"
#include "../assambler/instruction.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace hybridacc {

// Template binary format (from ha-asm output):
// Header: [num_patches:8][num_instructions:8]
// Patches: [offset:8][param_index:8] ...
// Instructions: [word:16] ...
static size_t calculateTemplateBinarySize(const PackageTemplate &tpl) {
    // Header: 2 bytes (num_patches + num_instructions)
    // Patches: 2 bytes per patch
    // Instructions: 2 bytes per instruction
    return 2 + (tpl.patches.size() * 2) + (tpl.instructions.size() * 2);
}

size_t Package::calculateBinarySize() const {
    // Package header: version(1) + num_templates(1) + offsets(2*N)
    size_t size = 2 + templates.size() * 2;

    // Template binaries
    for (const auto &tpl : templates) {
        size += calculateTemplateBinarySize(tpl);
    }

    return size;
}

std::vector<uint16_t> Package::calculateOffsets() const {
    std::vector<uint16_t> offsets;

    // First template starts after package header
    uint16_t currentOffset = 2 + templates.size() * 2;  // header size in bytes

    for (const auto &tpl : templates) {
        offsets.push_back(currentOffset);
        currentOffset += calculateTemplateBinarySize(tpl);
    }

    return offsets;
}

Packager::Packager() {}

void Packager::addTemplateFromAsm(const std::string &asmPath) {
    templates_.push_back(loadFromAsm(asmPath));
}

void Packager::addTemplateFromJson(const std::string &jsonPath) {
    templates_.push_back(loadFromJson(jsonPath));
}

Package Packager::createPackage() const {
    Package pkg;
    pkg.version = 1;
    pkg.templates = templates_;
    return pkg;
}

// Helper: Parse JSON manually (simple implementation)
static std::string extractJsonString(const std::string &json, const std::string &key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";

    pos = json.find("\"", pos);
    if (pos == std::string::npos) return "";

    size_t endPos = json.find("\"", pos + 1);
    if (endPos == std::string::npos) return "";

    return json.substr(pos + 1, endPos - pos - 1);
}

PackageTemplate Packager::loadFromJson(const std::string &jsonPath) const {
    std::ifstream file(jsonPath);
    if (!file) {
        throw AsmError("Cannot open JSON file: " + jsonPath);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string jsonContent = buffer.str();

    PackageTemplate tpl;
    tpl.sourcePath = jsonPath;

    // Extract template_name
    tpl.name = extractJsonString(jsonContent, "template_name");

    // Parse parameters array
    size_t paramsPos = jsonContent.find("\"parameters\"");
    if (paramsPos != std::string::npos) {
        size_t arrayStart = jsonContent.find("[", paramsPos);
        size_t arrayEnd = jsonContent.find("]", arrayStart);

        if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
            std::string paramsArray = jsonContent.substr(arrayStart + 1, arrayEnd - arrayStart - 1);

            // Simple parsing: look for objects
            size_t pos = 0;
            while (pos < paramsArray.size()) {
                size_t objStart = paramsArray.find("{", pos);
                if (objStart == std::string::npos) break;

                size_t objEnd = paramsArray.find("}", objStart);
                if (objEnd == std::string::npos) break;

                std::string obj = paramsArray.substr(objStart, objEnd - objStart + 1);

                TemplateParam param;
                param.name = extractJsonString(obj, "name");

                // Extract default value
                size_t defPos = obj.find("\"default\"");
                if (defPos != std::string::npos) {
                    size_t colonPos = obj.find(":", defPos);
                    size_t numEnd = obj.find_first_of(",}", colonPos);
                    std::string numStr = obj.substr(colonPos + 1, numEnd - colonPos - 1);
                    // Trim whitespace
                    numStr.erase(0, numStr.find_first_not_of(" \t\n\r"));
                    numStr.erase(numStr.find_last_not_of(" \t\n\r") + 1);
                    param.defaultValue = std::stoi(numStr);
                    param.hasDefault = true;
                }

                tpl.params.push_back(param);
                pos = objEnd + 1;
            }
        }
    }

    // Parse patches array
    size_t patchesPos = jsonContent.find("\"patches\"");
    if (patchesPos != std::string::npos) {
        size_t arrayStart = jsonContent.find("[", patchesPos);
        size_t arrayEnd = jsonContent.find("]", arrayStart);

        if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
            std::string patchesArray = jsonContent.substr(arrayStart + 1, arrayEnd - arrayStart - 1);

            size_t pos = 0;
            while (pos < patchesArray.size()) {
                size_t objStart = patchesArray.find("{", pos);
                if (objStart == std::string::npos) break;

                size_t objEnd = patchesArray.find("}", objStart);
                if (objEnd == std::string::npos) break;

                std::string obj = patchesArray.substr(objStart, objEnd - objStart + 1);

                int offset = 0, paramIndex = 0;

                size_t offsetPos = obj.find("\"offset\"");
                if (offsetPos != std::string::npos) {
                    size_t colonPos = obj.find(":", offsetPos);
                    size_t numEnd = obj.find_first_of(",}", colonPos);
                    std::string numStr = obj.substr(colonPos + 1, numEnd - colonPos - 1);
                    numStr.erase(0, numStr.find_first_not_of(" \t\n\r"));
                    numStr.erase(numStr.find_last_not_of(" \t\n\r") + 1);
                    offset = std::stoi(numStr);
                }

                size_t paramPos = obj.find("\"param_index\"");
                if (paramPos != std::string::npos) {
                    size_t colonPos = obj.find(":", paramPos);
                    size_t numEnd = obj.find_first_of(",}", colonPos);
                    std::string numStr = obj.substr(colonPos + 1, numEnd - colonPos - 1);
                    numStr.erase(0, numStr.find_first_not_of(" \t\n\r"));
                    numStr.erase(numStr.find_last_not_of(" \t\n\r") + 1);
                    paramIndex = std::stoi(numStr);
                }

                tpl.patches.emplace_back(offset, paramIndex);
                pos = objEnd + 1;
            }
        }
    }

    // Parse instructions array
    size_t instrsPos = jsonContent.find("\"instructions\"");
    if (instrsPos != std::string::npos) {
        size_t arrayStart = jsonContent.find("[", instrsPos);
        size_t arrayEnd = jsonContent.find("]", arrayStart);

        if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
            std::string instrsArray = jsonContent.substr(arrayStart + 1, arrayEnd - arrayStart - 1);

            size_t pos = 0;
            while (pos < instrsArray.size()) {
                size_t objStart = instrsArray.find("{", pos);
                if (objStart == std::string::npos) break;

                size_t objEnd = instrsArray.find("}", objStart);
                if (objEnd == std::string::npos) break;

                std::string obj = instrsArray.substr(objStart, objEnd - objStart + 1);

                // Extract "word" field
                std::string wordStr = extractJsonString(obj, "word");
                if (!wordStr.empty()) {
                    uint16_t word = 0;
                    if (wordStr.find("0x") == 0 || wordStr.find("0X") == 0) {
                        word = std::stoul(wordStr, nullptr, 16);
                    } else {
                        word = std::stoul(wordStr, nullptr, 10);
                    }
                    tpl.instructions.push_back(word);
                }

                pos = objEnd + 1;
            }
        }
    }

    return tpl;
}

PackageTemplate Packager::loadFromAsm(const std::string &asmPath) const {
    // Read ASM source
    std::string source = readAsm(asmPath);

    // Assemble template
    Assembler assembler;
    TemplateResult result = assembler.assembleTemplate(source, false);

    if (!result.isTemplate) {
        throw AsmError("File is not a template: " + asmPath);
    }

    PackageTemplate tpl;
    tpl.name = result.name;
    tpl.params = result.params;
    tpl.patches = result.patches;
    tpl.instructions = result.instructions;
    tpl.sourcePath = asmPath;

    return tpl;
}

void Packager::writePackageBinary(const std::string &path, const Package &pkg) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw AsmError("Cannot open output file: " + path);
    }

    // Calculate offsets
    std::vector<uint16_t> offsets = pkg.calculateOffsets();

    // Write header
    uint8_t version = pkg.version;
    uint8_t numTemplates = static_cast<uint8_t>(pkg.templates.size());

    file.write(reinterpret_cast<const char*>(&version), 1);
    file.write(reinterpret_cast<const char*>(&numTemplates), 1);

    // Write offsets (little-endian)
    for (uint16_t offset : offsets) {
        uint8_t lo = offset & 0xFF;
        uint8_t hi = (offset >> 8) & 0xFF;
        file.write(reinterpret_cast<const char*>(&lo), 1);
        file.write(reinterpret_cast<const char*>(&hi), 1);
    }

    // Write template binaries
    for (const auto &tpl : pkg.templates) {
        // Template header
        uint8_t numPatches = static_cast<uint8_t>(tpl.patches.size());
        uint8_t numInstructions = static_cast<uint8_t>(tpl.instructions.size());

        file.write(reinterpret_cast<const char*>(&numPatches), 1);
        file.write(reinterpret_cast<const char*>(&numInstructions), 1);

        // Patches
        for (const auto &patch : tpl.patches) {
            uint8_t offset = static_cast<uint8_t>(patch.offset);
            uint8_t paramIndex = static_cast<uint8_t>(patch.paramIndex);
            file.write(reinterpret_cast<const char*>(&offset), 1);
            file.write(reinterpret_cast<const char*>(&paramIndex), 1);
        }

        // Instructions (little-endian)
        for (uint16_t word : tpl.instructions) {
            uint8_t lo = word & 0xFF;
            uint8_t hi = (word >> 8) & 0xFF;
            file.write(reinterpret_cast<const char*>(&lo), 1);
            file.write(reinterpret_cast<const char*>(&hi), 1);
        }
    }
}

void Packager::writePackageJson(const std::string &path, const Package &pkg) const {
    std::ofstream file(path);
    if (!file) {
        throw AsmError("Cannot open output file: " + path);
    }

    std::vector<uint16_t> offsets = pkg.calculateOffsets();

    file << "{\n";
    file << "  \"package_version\": " << (int)pkg.version << ",\n";
    file << "  \"num_templates\": " << pkg.templates.size() << ",\n";
    file << "  \"total_binary_size\": " << pkg.calculateBinarySize() << ",\n";
    file << "  \"templates\": [\n";

    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];

        file << "    {\n";
        file << "      \"index\": " << i << ",\n";
        file << "      \"name\": \"" << tpl.name << "\",\n";
        file << "      \"offset\": " << offsets[i] << ",\n";
        file << "      \"binary_size\": " << calculateTemplateBinarySize(tpl) << ",\n";
        file << "      \"source\": \"" << tpl.sourcePath << "\",\n";

        // Parameters
        file << "      \"parameters\": [\n";
        for (size_t j = 0; j < tpl.params.size(); ++j) {
            const auto &param = tpl.params[j];
            file << "        {\n";
            file << "          \"index\": " << j << ",\n";
            file << "          \"name\": \"" << param.name << "\",\n";
            file << "          \"default\": " << param.defaultValue << "\n";
            file << "        }";
            if (j + 1 < tpl.params.size()) file << ",";
            file << "\n";
        }
        file << "      ],\n";

        // Patches
        file << "      \"patches\": [\n";
        for (size_t j = 0; j < tpl.patches.size(); ++j) {
            const auto &patch = tpl.patches[j];
            file << "        {\n";
            file << "          \"offset\": " << patch.offset << ",\n";
            file << "          \"param_index\": " << patch.paramIndex << "\n";
            file << "        }";
            if (j + 1 < tpl.patches.size()) file << ",";
            file << "\n";
        }
        file << "      ],\n";

        file << "      \"num_instructions\": " << tpl.instructions.size() << "\n";
        file << "    }";
        if (i + 1 < pkg.templates.size()) file << ",";
        file << "\n";
    }

    file << "  ]\n";
    file << "}\n";
}

void Packager::writePackageHeader(const std::string &path, const Package &pkg) const {
    std::ofstream file(path);
    if (!file) {
        throw AsmError("Cannot open output file: " + path);
    }

    std::vector<uint16_t> offsets = pkg.calculateOffsets();

    // Generate header guard
    std::string guardName = "HYBRIDACC_PACKAGE_H";

    file << "// Auto-generated package header for HybridAcc PE-ISA\n";
    file << "// Package version: " << (int)pkg.version << "\n";
    file << "// Number of templates: " << pkg.templates.size() << "\n";
    file << "// Total binary size: " << pkg.calculateBinarySize() << " bytes\n\n";

    file << "#ifndef " << guardName << "\n";
    file << "#define " << guardName << "\n\n";

    file << "#include <stdint.h>\n";
    file << "#include <string.h>\n\n";

    file << "#ifdef __cplusplus\n";
    file << "extern \"C\" {\n";
    file << "#endif\n\n";

    // ========== Base Data Structures ==========
    file << "// ========== Base Data Structures ==========\n\n";

    // Template patch structure
    file << "// Template patch information\n";
    file << "typedef struct {\n";
    file << "    uint8_t offset;        // Instruction offset to patch\n";
    file << "    uint8_t param_index;   // Parameter index\n";
    file << "} ha_template_patch_t;\n\n";

    // Package header structure
    file << "// Package header structure\n";
    file << "typedef struct {\n";
    file << "    uint8_t version;        // Package version\n";
    file << "    uint8_t num_templates;  // Number of templates\n";
    file << "} ha_package_header_t;\n\n";

    // ========== Package Information ==========
    file << "// ========== Package Information ==========\n\n";
    file << "#define HA_PACKAGE_VERSION " << (int)pkg.version << "\n";
    file << "#define HA_NUM_TEMPLATES " << pkg.templates.size() << "\n";
    file << "#define HA_TOTAL_BINARY_SIZE " << pkg.calculateBinarySize() << "\n\n";

    // ========== Template Indices ==========
    file << "// ========== Template Indices ==========\n\n";
    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];
        std::string enumName = "HA_TEMPLATE_";

        // Convert template name to uppercase and replace special chars
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                enumName += std::toupper(c);
            } else {
                enumName += '_';
            }
        }

        file << "#define " << enumName << " " << i << "\n";
    }
    file << "\n";

    // ========== Template-Specific Parameter Structures ==========
    file << "// ========== Template-Specific Parameter Structures ==========\n\n";

    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];

        // Generate safe identifier for template
        std::string safeName;
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                safeName += std::tolower(c);
            } else {
                safeName += '_';
            }
        }

        // Generate parameter structure for this template
        file << "// Parameters for template: " << tpl.name << "\n";
        file << "typedef struct {\n";

        if (tpl.params.empty()) {
            file << "    uint8_t _unused;  // No parameters\n";
        } else {
            for (const auto &param : tpl.params) {
                file << "    uint16_t " << param.name << ";";
                file << "  // default: " << param.defaultValue << "\n";
            }
        }

        file << "} ha_params_" << safeName << "_t;\n\n";
    }

    // ========== Template Information Structure ==========
    file << "// ========== Template Information Structure ==========\n\n";

    file << "// Template metadata\n";
    file << "typedef struct {\n";
    file << "    const char* name;              // Template name\n";
    file << "    uint8_t template_index;        // Template index in package\n";
    file << "    uint16_t offset;               // Offset in package binary\n";
    file << "    uint16_t binary_size;          // Size in bytes\n";
    file << "    uint8_t num_params;            // Number of parameters\n";
    file << "    uint8_t num_patches;           // Number of patches\n";
    file << "    uint8_t num_instructions;      // Number of instructions\n";
    file << "    const ha_template_patch_t* patches;  // Patch array\n";
    file << "    const void* default_params;    // Pointer to default parameters (cast to specific type)\n";
    file << "} ha_template_t;\n\n";

    // ========== Default Parameter Instances ==========
    file << "// ========== Default Parameter Instances ==========\n\n";

    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];

        // Generate safe identifier for template
        std::string safeName;
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                safeName += std::tolower(c);
            } else {
                safeName += '_';
            }
        }

        // Default parameters instance
        file << "// Default parameters for " << tpl.name << "\n";
        file << "static const ha_params_" << safeName << "_t ha_default_params_" << safeName << " = {\n";

        if (tpl.params.empty()) {
            file << "    ._unused = 0\n";
        } else {
            for (size_t j = 0; j < tpl.params.size(); ++j) {
                const auto &param = tpl.params[j];
                file << "    ." << param.name << " = " << param.defaultValue;
                if (j + 1 < tpl.params.size()) file << ",";
                file << "\n";
            }
        }

        file << "};\n\n";
    }

    // ========== Template Patches ==========
    file << "// ========== Template Patches ==========\n\n";

    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];

        if (tpl.patches.empty()) continue;

        // Generate safe identifier for template
        std::string safeName;
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                safeName += std::tolower(c);
            } else {
                safeName += '_';
            }
        }

        // Patch array
        file << "// Patches for " << tpl.name << "\n";
        file << "static const ha_template_patch_t ha_patches_" << safeName << "[] = {\n";
        for (size_t j = 0; j < tpl.patches.size(); ++j) {
            const auto &patch = tpl.patches[j];
            file << "    {" << (int)patch.offset << ", " << (int)patch.paramIndex << "}";
            if (j + 1 < tpl.patches.size()) file << ",";
            file << "\n";
        }
        file << "};\n\n";
    }

    // ========== Template Information Table ==========
    file << "// ========== Template Information Table ==========\n\n";
    file << "static const ha_template_t ha_template_table[] = {\n";

    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];

        // Generate safe identifier for template
        std::string safeName;
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                safeName += std::tolower(c);
            } else {
                safeName += '_';
            }
        }

        file << "    {\n";
        file << "        .name = \"" << tpl.name << "\",\n";
        file << "        .template_index = " << i << ",\n";
        file << "        .offset = " << offsets[i] << ",\n";
        file << "        .binary_size = " << calculateTemplateBinarySize(tpl) << ",\n";
        file << "        .num_params = " << tpl.params.size() << ",\n";
        file << "        .num_patches = " << tpl.patches.size() << ",\n";
        file << "        .num_instructions = " << tpl.instructions.size() << ",\n";

        if (tpl.patches.empty()) {
            file << "        .patches = NULL,\n";
        } else {
            file << "        .patches = ha_patches_" << safeName << ",\n";
        }

        file << "        .default_params = &ha_default_params_" << safeName << "\n";

        file << "    }";
        if (i + 1 < pkg.templates.size()) file << ",";
        file << "\n";
    }
    file << "};\n\n";

    // ========== Helper Functions ==========
    file << "// ========== Helper Functions ==========\n\n";

    // Get template info
    file << "// Get template information by index\n";
    file << "static inline const ha_template_t* ha_get_template_info(uint8_t template_index) {\n";
    file << "    if (template_index >= HA_NUM_TEMPLATES) {\n";
    file << "        return NULL;\n";
    file << "    }\n";
    file << "    return &ha_template_table[template_index];\n";
    file << "}\n\n";

    // Get template offset
    file << "// Get template offset in package binary\n";
    file << "static inline uint16_t ha_get_template_offset(uint8_t template_index) {\n";
    file << "    const ha_template_t* info = ha_get_template_info(template_index);\n";
    file << "    return info ? info->offset : 0;\n";
    file << "}\n\n";

    // Get template binary size
    file << "// Get template binary size\n";
    file << "static inline uint16_t ha_get_template_size(uint8_t template_index) {\n";
    file << "    const ha_template_t* info = ha_get_template_info(template_index);\n";
    file << "    return info ? info->binary_size : 0;\n";
    file << "}\n\n";

    // ========== Template-Specific Accessors ==========
    file << "// ========== Template-Specific Accessors ==========\n\n";

    for (size_t i = 0; i < pkg.templates.size(); ++i) {
        const auto &tpl = pkg.templates[i];

        // Generate safe identifier for template
        std::string safeName;
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                safeName += std::tolower(c);
            } else {
                safeName += '_';
            }
        }

        std::string enumName = "HA_TEMPLATE_";
        for (char c : tpl.name) {
            if (std::isalnum(c)) {
                enumName += std::toupper(c);
            } else {
                enumName += '_';
            }
        }

        file << "// Get default parameters for " << tpl.name << "\n";
        file << "static inline const ha_params_" << safeName << "_t* ha_get_default_params_" << safeName << "() {\n";
        file << "    return &ha_default_params_" << safeName << ";\n";
        file << "}\n\n";

        file << "// Get template info for " << tpl.name << "\n";
        file << "static inline const ha_template_t* ha_get_" << safeName << "_info() {\n";
        file << "    return &ha_template_table[" << enumName << "];\n";
        file << "}\n\n";
    }

    file << "#ifdef __cplusplus\n";
    file << "}\n";
    file << "#endif\n\n";

    file << "#endif // " << guardName << "\n";
}

} // namespace hybridacc
