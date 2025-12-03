#include "instruction.hpp"
#include "utils.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace hybridacc {

void writeBin(const std::string &path, const std::vector<uint16_t> &words){
    std::ofstream ofs(path, std::ios::binary);
    if(!ofs) throw AsmError("Cannot open output file: "+path);
    for(uint16_t w: words){
        ofs.write(reinterpret_cast<const char*>(&w), sizeof(w));
    }
}

void writeHex(const std::string &path, const std::vector<uint16_t> &words){
    std::ofstream ofs(path);
    if(!ofs) throw AsmError("Cannot open output file: "+path);
    for(size_t i=0;i<words.size();++i){
        ofs<<"0x"<<std::hex<<std::uppercase<< (int)words[i] <<"\n";
    }
}

std::vector<uint16_t> readBin(const std::string &path){
    std::ifstream ifs(path, std::ios::binary);
    if(!ifs) throw AsmError("Cannot open file: "+path);
    std::vector<uint16_t> v; uint16_t w; while(ifs.read(reinterpret_cast<char*>(&w), sizeof(w))) v.push_back(w); return v;
}

std::vector<uint16_t> readHex(const std::string &path){
    std::ifstream ifs(path);
    if(!ifs) throw AsmError("Cannot open file: "+path);
    std::vector<uint16_t> v; std::string line; while(std::getline(ifs,line)){
        line = trim(line);
        if(line.empty()) continue;
        if(line.rfind("0x",0)==0 || line.rfind("0X",0)==0) line = line.substr(2);
        unsigned long val = std::stoul(line, nullptr, 16);
        v.push_back(static_cast<uint16_t>(val & 0xFFFF));
    }
    return v;
}

// 新增：讀取 asm 原始碼
std::string readAsm(const std::string &path){
    std::ifstream ifs(path);
    if(!ifs) throw AsmError("Cannot open input: "+path);
    return std::string( (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>() );
}

/*
    Write template binary file
    Format: [header: 16bits] [patch_data...] [instruction_data...]
    Header: [15:8] patch_count, [7:0] template_len (instruction count)
    Each patch: [15:8] offset, [7:0] param_index
*/
void writeTemplateBin(const std::string &path, const TemplateResult &result) {
    std::ofstream ofs(path, std::ios::binary);
    if(!ofs) throw AsmError("Cannot open output file: "+path);

    // Write header: [15:8] patch_len, [7:0] template_len
    uint16_t header = ((result.patches.size() & 0xFF) << 8) | (result.instructions.size() & 0xFF);
    ofs.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write patch data
    for (const auto &patch : result.patches) {
        uint16_t patchWord = ((patch.offset & 0xFF) << 8) | (patch.paramIndex & 0xFF);
        ofs.write(reinterpret_cast<const char*>(&patchWord), sizeof(patchWord));
    }

    // Write instructions
    for (uint16_t w : result.instructions) {
        ofs.write(reinterpret_cast<const char*>(&w), sizeof(w));
    }
}

/*
    Write template hex file with annotations
*/
void writeTemplateHex(const std::string &path, const TemplateResult &result) {
    std::ofstream ofs(path);
    if(!ofs) throw AsmError("Cannot open output file: "+path);

    ofs << "# Template: " << result.name << "\n";
    ofs << "# Parameters: " << result.params.size() << "\n";
    for (size_t i = 0; i < result.params.size(); ++i) {
        ofs << "#   [" << i << "] " << result.params[i].name;
        if (result.params[i].hasDefault) {
            ofs << " = " << result.params[i].defaultValue;
        }
        ofs << "\n";
    }
    ofs << "# Patches: " << result.patches.size() << "\n";
    ofs << "\n";

    // Header: [15:8] patch_len, [7:0] template_len
    uint16_t header = ((result.patches.size() & 0xFF) << 8) | (result.instructions.size() & 0xFF);
    ofs << "# Header (patch_len=" << result.patches.size() << ", template_len=" << result.instructions.size() << ")\n";
    ofs << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (int)header << "\n\n";

    // Patches
    if (!result.patches.empty()) {
        ofs << "# Patch data (offset, param_index)\n";
        for (const auto &patch : result.patches) {
            uint16_t patchWord = ((patch.offset & 0xFF) << 8) | (patch.paramIndex & 0xFF);
            ofs << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (int)patchWord;
            ofs << "  # offset=" << std::dec << patch.offset << ", param=" << patch.paramIndex << "\n";
        }
        ofs << "\n";
    }

    // Instructions
    ofs << "# Template instructions (" << std::dec << result.instructions.size() << " words)\n";
    for (size_t i = 0; i < result.instructions.size(); ++i) {
        ofs << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (int)result.instructions[i] << "\n";
    }
}

/*
    Write template JSON file
*/
void writeTemplateJson(const std::string &path, const TemplateResult &result) {
    Disassembler d;
    std::ofstream ofs;
    if (path != "-") ofs.open(path);
    std::ostream &os = (path == "-") ? std::cout : ofs;
    if (!os.good()) throw AsmError("Cannot open json output: " + path);

    os << "{\n";
    os << "  \"template_name\": \"" << result.name << "\",\n";
    os << "  \"parameters\": [\n";
    for (size_t i = 0; i < result.params.size(); ++i) {
        os << "    {\"index\": " << i << ", \"name\": \"" << result.params[i].name << "\"";
        if (result.params[i].hasDefault) {
            os << ", \"default\": " << result.params[i].defaultValue;
        }
        os << "}";
        if (i + 1 < result.params.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"patches\": [\n";
    for (size_t i = 0; i < result.patches.size(); ++i) {
        os << "    {\"offset\": " << result.patches[i].offset
           << ", \"param_index\": " << result.patches[i].paramIndex << "}";
        if (i + 1 < result.patches.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"instructions\": [\n";
    for (size_t i = 0; i < result.instructions.size(); ++i) {
        auto w = result.instructions[i];
        std::ostringstream dis;
        dis << d.disasmWord(w);
        os << "    {\"index\": " << i << ", \"word\": \"0x" << std::uppercase << std::hex
           << std::setw(4) << std::setfill('0') << (int)w << "\", \"dec\": " << std::dec << (int)w
           << ", \"disasm\": \"";

        // Escape quotes/backslashes
        std::string s = dis.str();
        for (char c : s) {
            if (c == '\\' || c == '\"') os << '\\';
            if (c == '\n') { os << "\\n"; } else if (c != '\n') os << c;
        }
        os << "\"}";
        if (i + 1 < result.instructions.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";
    os << "}\n";
}

/*
    Write template disassembly file
*/
void writeTemplateDisasm(const std::string &path, const TemplateResult &result) {
    Disassembler d;
    std::ofstream ofs(path);
    if (!ofs) throw AsmError("Cannot open disasm output: " + path);

    ofs << "# Template: " << result.name << "\n";
    ofs << "# Parameters: " << result.params.size() << "\n";
    for (size_t i = 0; i < result.params.size(); ++i) {
        ofs << "#   [" << i << "] " << result.params[i].name;
        if (result.params[i].hasDefault) {
            ofs << " = " << result.params[i].defaultValue;
        }
        ofs << "\n";
    }
    ofs << "# Patches: " << result.patches.size() << "\n";
    for (const auto &patch : result.patches) {
        ofs << "#   offset=" << patch.offset << " -> param[" << patch.paramIndex << "] ("
            << result.params[patch.paramIndex].name << ")\n";
    }
    ofs << "\n";

    ofs << "# Disassembly:\n";
    for (size_t i = 0; i < result.instructions.size(); ++i) {
        ofs << std::setw(4) << std::setfill(' ') << i << ": " << d.disasmWord(result.instructions[i]) << "\n";
    }
}

} // namespace hybridacc
