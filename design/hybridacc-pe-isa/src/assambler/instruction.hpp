// 初版 HybridAcc ISA 組譯/反組譯工具共用定義
// 參考: doc/Hybridacc PE.md
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <functional>
#include <sstream>
#include <algorithm>
#include <map>
#include <optional>

namespace hybridacc {

// 16-bit 指令格式 (bit 0 = loop_end flag)
// [15:13] func3, [12] func1, [11:5] payload, [4:3] funct2, [2:1] opcode, [0] loop_end
// payload 7 bits 在不同指令中再細分

struct AsmError : public std::runtime_error { using std::runtime_error::runtime_error; };

inline std::string trim(const std::string &s){
    size_t b = s.find_first_not_of(" \t\r\n");
    if(b==std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b,e-b+1);
}

inline std::vector<std::string> splitOperands(const std::string &rest){
    std::vector<std::string> out;
    std::string cur;
    int par=0;
    for(char c: rest){
        if(c==',' && par==0){
            std::string t = trim(cur);
            if(!t.empty()) out.push_back(t);
            cur.clear();
        } else { cur.push_back(c); }
    }
    std::string t = trim(cur);
    if(!t.empty()) out.push_back(t);
    return out;
}

inline int parseInt(const std::string &tok){
    if(tok.rfind("0x",0)==0 || tok.rfind("0X",0)==0) return std::stoi(tok,nullptr,16);
    if(tok.rfind("0b",0)==0 || tok.rfind("0B",0)==0) return std::stoi(tok.substr(2),nullptr,2);
    return std::stoi(tok, nullptr, 10);
}

struct EncoderCtx { std::vector<uint16_t> words; };

using EncodeFn = std::function<void(const std::vector<std::string>&, EncoderCtx&)>;

struct MnemonicDef { std::string name; EncodeFn fn; };

struct Patch { enum Type{JUMP} type; int index; std::string label; };
// Patch 用於 JUMP 指令，記錄目標 label 及其 index

// Template parameter definition
struct TemplateParam {
    std::string name;
    int defaultValue;
    bool hasDefault;

    TemplateParam() : name(""), defaultValue(0), hasDefault(false) {}
    TemplateParam(const std::string& n, int v, bool hd)
        : name(n), defaultValue(v), hasDefault(hd) {}
};

// Template parameter patch information
struct TemplatePatch {
    int offset;        // instruction offset in template code
    int paramIndex;    // parameter index in template definition

    TemplatePatch(int o, int pi) : offset(o), paramIndex(pi) {}
};

// Template compilation result
struct TemplateResult {
    std::string name;
    std::vector<TemplateParam> params;
    std::vector<TemplatePatch> patches;
    std::vector<uint16_t> instructions;
    bool isTemplate;

    TemplateResult() : name(""), isTemplate(false) {}
};

// 寫入通用欄位 helper
inline uint16_t makeBase(uint8_t opcode, uint8_t funct2){
    uint16_t v=0;
    v |= (opcode & 0x3) << 1;
    v |= (funct2 & 0x3) << 3;
    return v;
}

inline void setFunc3Func1(uint16_t &v, int func3, int func1){
    v |= (func3 & 0x7) << 13;
    v |= (func1 & 0x1) << 12;
}
inline void setPayload(uint16_t &v, int payload){
    v |= (payload & 0x7F) << 5;
}

// 解析寄存器 (P*, VP*, T*, VT*) 轉數值, 僅檢查範圍不做型別差異
inline int parseRegIndex(const std::string &r){
    if(r.size()<2) throw AsmError("Bad register name: "+r);
    std::string pfx;
    int i=0;
    while(i<(int)r.size() && std::isalpha((unsigned char)r[i])){
        pfx.push_back(r[i]);
        ++i;
    }
    std::string up = pfx;
    std::transform(up.begin(),up.end(),up.begin(),::toupper);
    int id = parseInt(r.substr(i));
    if(up=="P") {
        if(id<0 || id>31) throw AsmError("Psum Register out of range: "+r);
    }
    if(up=="T") {
        if(id<0 || id>11) throw AsmError("Transform register out of range: "+r);
    }
    if(up=="VP") {
        if(id<0 || id>31) throw AsmError("Vector psum register out of range: "+r);
    }
    if(up=="VT") {
        if(id<0 || id>2) throw AsmError("Vector transform register out of range: "+r);
    }
    return id;
}

inline bool parseSpecialToken(const std::string &tok, int &value){
    std::string u=tok; std::transform(u.begin(),u.end(),u.begin(),::toupper);
    if(u=="VTRST"){ value=3; return true; } // 最大 2-bit 值 3 代表 reset
    if(u=="VPRST"){ value=31; return true; } // 最大 5-bit 值 31 代表 reset
    if(u=="PRST"){ value=31; return true; } // 最大 5-bit 值 31 代表 reset
    if(u.size()==2 && u[0]=='K' && (u[1]=='3' || u[1]=='5' || u[1]=='7')){
        value = (u[1] - '0');
        return true;
    }
    return false;
}

class Assembler {
public:
    Assembler();
    std::vector<uint16_t> assemble(const std::string &source, bool verbose);
    TemplateResult assembleTemplate(const std::string &source, bool verbose);
private:
    std::unordered_map<std::string, MnemonicDef> table;
    std::map<std::string,int> labels; // label -> instruction index
    std::vector<Patch> patches;

    // Template support
    std::map<std::string, int> templateParams; // param name -> index during parsing
    std::vector<TemplatePatch> templatePatches; // patches for template parameters

    void applyPatches(std::vector<uint16_t> &words);
    bool parseTemplateHeader(const std::string &line, std::string &templateName, std::vector<TemplateParam> &params);
    std::optional<int> extractTemplateParam(const std::string &operand);
};

class Disassembler {
public:
    std::string disasmWord(uint16_t w) const;
};

} // namespace hybridacc
