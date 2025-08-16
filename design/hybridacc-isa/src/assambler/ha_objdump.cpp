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

int main(int argc, char** argv){
    if(argc<2){
        std::cerr<<"Usage: ha-objdump <input.bin|input.hex>\n";
        return 1;
    }
    std::string in=argv[1];
    try {
        std::vector<uint16_t> words;
        if(endsWith(in, ".bin")) words = readBin(in);
        else words = readHex(in);
        Disassembler d;
        for(size_t i=0;i<words.size();++i){
            std::cout<<std::setw(4)<<i<<": "<<d.disasmWord(words[i])<<"\n";
        }
    } catch(const std::exception &e){
        std::cerr<<"Error: "<<e.what()<<"\n"; return 1;
    }
    return 0;
}
