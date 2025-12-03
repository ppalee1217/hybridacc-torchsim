#include "instruction.hpp"
#include "utils.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <cctype>

using namespace hybridacc;

static bool endsWith(const std::string &s, const std::string &suf){
    return s.size()>=suf.size() && s.compare(s.size()-suf.size(), suf.size(), suf)==0;
}

static void disasmTemplate(const std::vector<uint16_t> &words) {
    if (words.empty()) {
        std::cerr << "Error: Empty template binary\n";
        return;
    }

    // Parse header: [15:8] patch_len, [7:0] template_len (instruction count)
    uint16_t header = words[0];
    int patchLen = (header >> 8) & 0xFF;
    int templateLen = header & 0xFF;

    std::cout << "# Template Binary Format\n";
    std::cout << "# Header: 0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << header << std::dec << "\n";
    std::cout << "#   Patch count: " << patchLen << "\n";
    std::cout << "#   Template length: " << templateLen << " instructions\n";
    std::cout << "\n";

    // Parse patches: [15:8] offset, [7:0] param_index
    if (patchLen > 0) {
        std::cout << "# Patch Information:\n";
        for (int i = 0; i < patchLen; ++i) {
            if (1 + i >= (int)words.size()) {
                std::cerr << "Error: Insufficient data for patches\n";
                return;
            }
            uint16_t patchWord = words[1 + i];
            int offset = (patchWord >> 8) & 0xFF;
            int paramIndex = patchWord & 0xFF;
            std::cout << "#   [" << i << "] offset=" << std::setw(3) << offset
                      << ", param_index=" << paramIndex
                      << " (0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << patchWord << std::dec << ")\n";
        }
        std::cout << "\n";
    }

    // Disassemble instructions
    int instrStart = 1 + patchLen;
    int instrCount = words.size() - instrStart;

    // Verify template length
    if (templateLen > 0 && instrCount != templateLen) {
        std::cerr << "# Warning: Expected " << templateLen << " instructions, but found " << instrCount << "\n\n";
    }

    if (instrCount <= 0) {
        std::cerr << "Error: No instructions in template binary\n";
        return;
    }

    std::cout << "# Template Instructions (" << instrCount << " words):\n";
    Disassembler d;
    for (int i = 0; i < instrCount; ++i) {
        int wordIndex = instrStart + i;
        std::cout << std::setw(4) << std::setfill(' ') << i << ": "
                  << d.disasmWord(words[wordIndex]) << "\n";
    }
}

int main(int argc, char** argv){
    if(argc<2){
        std::cerr<<"Usage: ha-objdump <input.bin|input.hex> [--template]\n";
        std::cerr<<"  --template: Treat input as template binary format\n";
        return 1;
    }
    std::string in=argv[1];
    bool isTemplate = false;

    // Check for --template flag
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--template" || arg == "-t") {
            isTemplate = true;
        }
    }

    // Auto-detect template format from filename
    if (in.find("_temp.") != std::string::npos) {
        isTemplate = true;
        std::cout << "# Auto-detected template binary format\n";
    }

    try {
        std::vector<uint16_t> words;
        if(endsWith(in, ".bin")) words = readBin(in);
        else words = readHex(in);

        if (isTemplate) {
            disasmTemplate(words);
        } else {
            Disassembler d;
            for(size_t i=0;i<words.size();++i){
                std::cout<<std::setw(4)<<i<<": "<<d.disasmWord(words[i])<<"\n";
            }
        }
    } catch(const std::exception &e){
        std::cerr<<"Error: "<<e.what()<<"\n"; return 1;
    }
    return 0;
}
