#include "instruction.hpp"
#include "utils.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>

using namespace hybridacc;

static void writeJson(const std::string &path, const std::vector<uint16_t> &words){
    Disassembler d; std::ofstream ofs; if(path!="-") ofs.open(path); std::ostream &os = (path=="-")? std::cout : ofs;
    if(!os.good()) throw AsmError("Cannot open json output: "+path);
    os << "[\n"; for(size_t i=0;i<words.size();++i){
        auto w = words[i];
        std::ostringstream dis; dis<<d.disasmWord(w);
        os << "  {\"index\": "<<i<<", \"word\": \"0x"<<std::uppercase<<std::hex<<std::setw(4)<<std::setfill('0')<< (int)w
           <<"\", \"dec\": "<< std::dec << (int)w << ", \"disasm\": \"";
        // escape quotes/backslashes
        std::string s = dis.str();
        for(char c: s){ if(c=='\\' || c=='\"') os<<'\\'; if(c=='\n') { os << "\\n"; } else if(c!='\n') os<<c; }
        os << "\"}" << (i+1==words.size()?"\n":" ,\n");
    }
    os << "]\n";
}

static bool isTemplateSource(const std::string &source) {
    // Check if source contains .template directive
    return source.find(".template") != std::string::npos;
}

int main(int argc, char** argv){
    if(argc<2){
        std::cerr<<"Usage: ha-asm <input.asm> [-o output.bin] [--hex output.hex] [--json out.json|-] [--disasm output.disasm] [--verbose] \n";
        std::cerr<<"  Template files will generate *_temp.bin/hex/json/disasm outputs\n";
        return 1;
    }
    std::string in = argv[1];
    std::string outBin;
    std::string outHex;
    std::string outJson;
    std::string outDisasm;
    bool verbose = false;
    for(int i=2;i<argc;i++){
        std::string a = argv[i];
        if(a=="-o" && i+1<argc){ outBin = argv[++i]; }
        else if(a=="--hex" && i+1<argc){ outHex = argv[++i]; }
        else if(a=="--json" && i+1<argc){ outJson = argv[++i]; }
        else if(a=="--disasm" && i+1<argc){ outDisasm = argv[++i]; }
        else if(a=="--verbose"){ verbose = true; }
    }

    try {
        // Read source
        std::string src = readAsm(in);

        // Check if it's a template
        if (isTemplateSource(src)) {
            if(verbose) std::cout << "Detected template source code\n";

            // Set default outputs with _temp suffix if not specified
            std::string baseName = in;
            size_t dotPos = baseName.rfind('.');
            if (dotPos != std::string::npos) {
                baseName = baseName.substr(0, dotPos);
            }

            // Assemble template
            Assembler asmblr;
            auto result = asmblr.assembleTemplate(src, verbose);

            if (!result.isTemplate) {
                std::cerr << "Warning: .template directive found but parsing failed, treating as regular assembly\n";
                auto words = asmblr.assemble(src, verbose);
                if(!outBin.empty()) writeBin(outBin, words);
                if(!outHex.empty()) writeHex(outHex, words);
                if(!outJson.empty()) writeJson(outJson, words);
            } else {
                // Write template outputs
                if(!outBin.empty()) writeTemplateBin(outBin, result);
                if(!outHex.empty()) writeTemplateHex(outHex, result);
                if(!outJson.empty()) writeTemplateJson(outJson, result);
                if(!outDisasm.empty()) writeTemplateDisasm(outDisasm, result);

                if(verbose) {
                    std::cout << "Template '" << result.name << "' compiled successfully:\n";
                    std::cout << "  Parameters: " << result.params.size() << "\n";
                    std::cout << "  Patches: " << result.patches.size() << "\n";
                    std::cout << "  Instructions: " << result.instructions.size() << "\n";
                    if (!outBin.empty()) std::cout << "  Binary: " << outBin << "\n";
                    if (!outHex.empty()) std::cout << "  Hex: " << outHex << "\n";
                    if (!outJson.empty()) std::cout << "  JSON: " << outJson << "\n";
                    if (!outDisasm.empty()) std::cout << "  Disasm: " << outDisasm << "\n";
                }
            }
        } else {
            // Regular assembly
            if(outBin.empty() && outHex.empty() && outJson.empty()) outHex = in + ".hex";

            Assembler asmblr;
            auto words = asmblr.assemble(src, verbose);
            if(!outBin.empty()) writeBin(outBin, words);
            if(!outHex.empty()) writeHex(outHex, words);
            if(!outJson.empty()) writeJson(outJson, words);
        }
    } catch(const std::exception &e){
        std::cerr<<"Error: "<<e.what()<<"\n"; return 1;
    }
    return 0;
}
