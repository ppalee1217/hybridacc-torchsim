#include "instruction.hpp"
#include "utils.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>

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

} // namespace hybridacc
