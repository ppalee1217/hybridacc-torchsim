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

int main(int argc, char** argv){
    if(argc<2){
        std::cerr<<"Usage: ha-asm <input.asm> [-o output.bin] [--hex output.hex] [--json out.json|-] [--verbose] \n";
        return 1;
    }
    std::string in = argv[1];
    std::string outBin;
    std::string outHex;
    std::string outJson;
    bool verbose = false;
    for(int i=2;i<argc;i++){
        std::string a = argv[i];
        if(a=="-o" && i+1<argc){ outBin = argv[++i]; }
        else if(a=="--hex" && i+1<argc){ outHex = argv[++i]; }
        else if(a=="--json" && i+1<argc){ outJson = argv[++i]; }
        else if(a=="--verbose"){ verbose = true; }
    }
    if(outBin.empty() && outHex.empty() && outJson.empty()) outHex = in + ".hex";

    try {
        // 使用 readAsm 讀取來源
        std::string src = readAsm(in);
        Assembler asmblr;
        auto words = asmblr.assemble(src, verbose);
        if(!outBin.empty()) writeBin(outBin, words);
        if(!outHex.empty()) writeHex(outHex, words);
        if(!outJson.empty()) writeJson(outJson, words);
    } catch(const std::exception &e){
        std::cerr<<"Error: "<<e.what()<<"\n"; return 1;
    }
    return 0;
}
