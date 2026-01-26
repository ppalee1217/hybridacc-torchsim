#include "instruction.hpp"
#include <fstream>
#include <iostream>
#include <cctype>

namespace hybridacc {

/*
    Splits the operand string into individual tokens.
    Supports both comma-separated and space-separated formats.
    Returns a vector of strings.
*/
Assembler::Assembler(){
    // 建立對應表 (大小寫不敏感，先轉大寫再查)
    auto add = [&](const std::string &mn, EncodeFn fn){
        std::string u=mn;
        std::transform(u.begin(),u.end(),u.begin(),::toupper);
        table[u] = {u, fn};
    };

    // Pseudo: LOOPEND -> set previous bit0
    add("LOOPEND", [=](const std::vector<std::string>&, EncoderCtx &ctx){
        if(ctx.words.empty()) throw AsmError("LOOPEND without previous instruction");
        if(ctx.words.back() & 0x1) ctx.words.push_back(0x4|0x1); // NOP with LOOPEND
        else ctx.words.back() |= 0x1; // loop-end flag
    });

    // LDMA.ADDR start_addr (opcode=00 f2=01 f1=0)
    add("LDMA.ADDR", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LDMA.ADDR expects 1 operand");
        int addr = parseInt(ops[0]);
        if(addr < 0 || addr > 0x3FF) throw AsmError("start_addr out of range (0..1023)");
        uint16_t w = makeBase(0, 1); // opcode=00 funct2=01
        w |= ((addr >> 7) & 0x7) << 13;      // bits 15:13 = addr[9:7]
        // bit12 func1=0 (LDMA ADDR)
        w |= (addr & 0x1) << 11;             // bit11 = addr[0]
        w |= ((addr >> 1) & 0x3F) << 5;      // bits 10:5 = addr[6:1]
        ctx.words.push_back(w);
    });

    // LDMA.LEN len (opcode=00 f2=01 f1=1)
    add("LDMA.LEN", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LDMA.LEN expects 1 operand");
        int len = parseInt(ops[0]);
        if(len < 0 || len > 0x3FF) throw AsmError("len out of range (0..1023)");
        uint16_t w = makeBase(0, 1); // opcode=00 funct2=01
        w |= ((len >> 7) & 0x7) << 13;       // bits 15:13 = len[9:7]
        w |= 1 << 12;                        // func1=1 (LDMA LEN)
        w |= (len & 0x1) << 11;              // bit11 = len[0]
        w |= ((len >> 1) & 0x3F) << 5;       // bits 10:5 = len[6:1]
        ctx.words.push_back(w);
    });

    // SDMA.ADDR start_addr (opcode=00 f2=00 f1=0)
    add("SDMA.ADDR", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.ADDR expects 1 operand");
        int addr = parseInt(ops[0]);
        if(addr < 0 || addr > 0x3FF) throw AsmError("start_addr out of range (0..1023)");
        uint16_t w = makeBase(0, 0); // opcode=00 funct2=00
        w |= ((addr >> 7) & 0x7) << 13;      // bits 15:13 = addr[9:7]
        // bit12 func1=0 (SDMA ADDR)
        w |= (addr & 0x1) << 11;             // bit11 = addr[0]
        w |= ((addr >> 1) & 0x3F) << 5;      // bits 10:5 = addr[6:1]
        ctx.words.push_back(w);
    });

    // SDMA.LEN len (opcode=00 f2=00 f1=1)
    add("SDMA.LEN", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.LEN expects 1 operand");
        int len = parseInt(ops[0]);
        if(len < 0 || len > 0x3FF) throw AsmError("len out of range (0..1023)");
        uint16_t w = makeBase(0, 0); // opcode=00 funct2=00
        w |= ((len >> 7) & 0x7) << 13;       // bits 15:13 = len[9:7]
        w |= 1 << 12;                        // func1=1 (SDMA LEN)
        w |= (len & 0x1) << 11;              // bit11 = len[0]
        w |= ((len >> 1) & 0x3F) << 5;       // bits 10:5 = len[6:1]
        ctx.words.push_back(w);
    });

    // LDMA.LOOP count (opcode=01 f2=01 f1=0)
    add("LDMA.LOOP", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LDMA.LOOP expects 1 operand");
        int count = parseInt(ops[0]);
        if(count < 1 || count > 1024) throw AsmError("count out of range (1..1024) for 0-based encoding");
        int val = count - 1; // 0-based encoding
        uint16_t w = makeBase(1, 1); // opcode=01 funct2=01
        w |= ((val >> 7) & 0x7) << 13;     // bits 15:13
        // bit12 func1=0
        w |= (val & 0x1) << 11;            // bit11
        w |= ((val >> 1) & 0x3F) << 5;     // bits 10:5
        ctx.words.push_back(w);
    });

    // SDMA.LOOP count (opcode=01 f2=01 f1=1)
    add("SDMA.LOOP", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.LOOP expects 1 operand");
        int count = parseInt(ops[0]);
        if(count < 1 || count > 1024) throw AsmError("count out of range (1..1024) for 0-based encoding");
        int val = count - 1; // 0-based encoding
        uint16_t w = makeBase(1, 1); // opcode=01 funct2=01
        w |= ((val >> 7) & 0x7) << 13;     // bits 15:13
        w |= 1 << 12;                        // func1=1
        w |= (val & 0x1) << 11;            // bit11
        w |= ((val >> 1) & 0x3F) << 5;     // bits 10:5
        ctx.words.push_back(w);
    });

    auto addDMALoad = [&](const std::string &mn, int func3){
        add(mn, [=](const std::vector<std::string>&ops, EncoderCtx &ctx){
            if(ops.size()!=1) throw AsmError(mn+" expects 1 operand stride");
            int stride = parseInt(ops[0]);
            if(stride<0 || stride>7) throw AsmError("stride out of range(0..7)");
            uint16_t w = makeBase(0, 2); // opcode=00 funct2=11
            // func3 bits [15:13]
            w |= (func3 & 0x7) << 13;
            // stride bits [12:10]
            w |= (stride & 0x7) << 10;
            ctx.words.push_back(w);
        });
    };
    addDMALoad("LDMA.LB", 0);
    addDMALoad("LDMA.LH", 1);
    addDMALoad("LDMA.LW", 2);
    addDMALoad("LDMA.LD", 3);
    addDMALoad("LDMA.LBB",4);
    addDMALoad("LDMA.LHB",5);
    addDMALoad("LDMA.LWB",6);

    // SDMA.SD stride (store double-word) opcode=00 funct2=11 func3=011 stride bits [12:10]
    add("SDMA.SD", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.SD expects 1 operand");
        int stride = parseInt(ops[0]); if(stride<0||stride>7) throw AsmError("stride out of range");
        uint16_t w = makeBase(0, 3); // opcode=00 funct2=11 func3=011
        w |= (0x3) << 13; // func3=011
        w |= (stride & 0x7) << 10; // bits 12:10
        ctx.words.push_back(w);
    });

    // TSTORE trd (Opcode 01 Function 00 func3=000)
    add("TSTORE", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("TSTORE expects 1 operand (trd)");
        int trd = parseRegIndex(ops[0]);
        uint16_t w = makeBase(1, 0); // opcode=01 funct2=00
        // func3=000 already 0
        w |= (trd & 0xf) << 5; // [8:5]
        ctx.words.push_back(w);
    });
    // TSHIFT (Opcode 01 Function 00 func3=001)
    add("TSHIFT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("TSHIFT expects kernel_size (K3/K5/K7 or 3/5/7)");
        int k;
        if(!parseSpecialToken(ops[0], k)) {
            try { k = parseInt(ops[0]); } catch(...) { throw AsmError("Invalid kernel size token"); }
        }
        int code=-1;
        if(k==3) code=0; else if(k==5) code=1; else if(k==7) code=2; else throw AsmError("Unsupported kernel size (3/5/7)");
        uint16_t w = makeBase(1, 0); // opcode=01 funct2=00
        w |= (1)<<13; /*func3=001*/
        w |= (code & 0x7) << 10;
        ctx.words.push_back(w);
    });

    auto addVMx = [&](const std::string &mn, int func3, bool hasN, bool regCtrl, bool mul){
        add(mn, [=](const std::vector<std::string>&ops, EncoderCtx &ctx){
            uint16_t w = makeBase(2, 1); // opcode=10 funct2=01
            int func1 = 0;
            if(hasN && mn.back()=='N') func1=1; // 尾碼 N
            setFunc3Func1(w, func3, func1);
            if(!regCtrl){
                if(ops.size()!=2) throw AsmError(mn+" expects 2 operands");
                int prd = parseRegIndex(ops[0]);
                int vtrs;
                try {
                    vtrs = parseRegIndex(ops[1]);
                } catch(...){
                    int tmp; if(parseSpecialToken(ops[1], tmp)) vtrs=tmp; else throw;
                }
                if(prd<0||prd>31) throw AsmError("prd out of range");
                if(vtrs<0||vtrs>2) throw AsmError("vtrs out of range (0..2)");
                w |= (prd & 0x1F) << 5; w |= (vtrs & 0x3) << 10;
            } else {
                if(ops.size()!=2) throw AsmError(mn+" expects 2 stride operands");
                int ps; int vts;
                try {
                    ps = parseInt(ops[0]);
                } catch(...){
                    int tmp; if(parseSpecialToken(ops[0], tmp)) ps=tmp; else throw;
                }
                try {
                    vts = parseInt(ops[1]);
                } catch(...){
                    int tmp;
                    if(parseSpecialToken(ops[1], tmp)) vts=tmp; else throw;
                }
                if(ps<0||ps>31) throw AsmError("pstride out of range");
                if(vts<0||vts>3) throw AsmError("vtstride out of range (0..2)");
                w |= (ps & 0x1F) << 5; w |= (vts & 0x3) << 10; // vtstride 2 bits
            }
            ctx.words.push_back(w);
        });
    };
    addVMx("VMAC", 0, true, false, false);
    addVMx("VMACN",0, true, false, false);
    addVMx("VMACR",1, true, true, false);
    addVMx("VMACRN",1, true, true, false);
    addVMx("VMUL",2, true, false, true);
    addVMx("VMULN",2, true, false, true);
    addVMx("VMULR",3, true, true, true);
    addVMx("VMULRN",3, true, true, true);

    add("VPSUM", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("VPSUM expects vprs");
        int vprs = parseRegIndex(ops[0]);
        if(vprs<0||vprs>31) throw AsmError("vprs out of range");
        uint16_t w = makeBase(2, 1); // opcode=10 funct2=01
        w |= (vprs & 0x1F) << 5; // bits 9:5
        w |= (0x4) << 13; // func3=100
        ctx.words.push_back(w);
    });
    add("VPSUMR", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("VPSUMR expects vpstride");
        int vpstride = parseInt(ops[0]);
        if(vpstride<0||vpstride>31) throw AsmError("vpstride out of range");
        uint16_t w = makeBase(2, 1); // opcode=10 funct2=01
        w |= (vpstride & 0x1F) << 5; // bits 9:5
        w |= (0x5)<<13; // func3=101
        ctx.words.push_back(w);
    });

    // J imm pattern same as LDMA.LEN
    add("J", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("J expects 1 immediate/label");
        int imm=-1;
        bool isLabel=false;
        try {
            imm = parseInt(ops[0]);
        } catch(...){
            isLabel=true;
        }
        uint16_t w = makeBase(1, 2); // opcode=01 funct2=10
        if(!isLabel){
            if(imm<0||imm>0x7FF) throw AsmError("imm out of range (0..2047)");
            if(imm & 0x1) throw AsmError("J immediate must be even (byte address, 2-byte aligned)");
            int b_9_7 = (imm >> 7) & 0x7;
            int b_10 = (imm >> 10) &1;
            int b_0 = imm &1;
            int b_6_1 = (imm>>1)&0x3F;
            w |= b_9_7<<13; w |= b_10<<12; w |= b_0<<11; w |= b_6_1<<5;
        } else {
            patches.push_back({Patch::JUMP,(int)ctx.words.size(), ops[0]});
        }
        ctx.words.push_back(w);
    });

    // LOOPIN loop_count pattern bits 9:7 -> [15:13]; bits 6:1 + bit0 -> [11:5]; func1=0 at bit12, funct2=11 opcode=01 func1=0, func3= loop_count[9:7]
    add("LOOPIN", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LOOPIN expects loop_count");
        int lc = parseInt(ops[0]); if(lc<1||lc>1024) throw AsmError("loop_count out of range (1..1024) for 0-based encoding");
        int val = lc - 1; // 0-based encoding
        uint16_t w = makeBase(1, 3); // opcode=01 funct2=11
        int b_9_7=(val>>7)&0x7;
        int b_0 = val &1;
        int b_6_1=(val>>1)&0x3F;
        w |= b_9_7<<13; // func3 region
        // bit12 func1=0 already 0
        w |= b_0 << 11;
        w |= b_6_1 <<5;
        ctx.words.push_back(w);
    });
    add("LOOPBREAK", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("LOOPBREAK no operand");
        uint16_t w = makeBase(1, 3); // opcode=01 funct2=11
        // func1=1 at bit12; set it; func3 maybe 0
        w |= 1<<12; ctx.words.push_back(w);
    });

    // System level
    add("NOP", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("NOP no operands");
        uint16_t w = makeBase(2, 0);
        ctx.words.push_back(w);
    });
    add("SWAPDM", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("SWAPDM no operands");
        uint16_t w = makeBase(3, 3); // opcode=11 funct2=11
        w |= (4) << 13; // func3 = 100
        ctx.words.push_back(w);
    });
    add("HALT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("HALT no operands");
        uint16_t w = makeBase(3, 3); // opcode=11 funct2=11
        // func3 = 000
        ctx.words.push_back(w);
    });
    add("CLEAR.T", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("CLEAR.T no operands");
        uint16_t w = makeBase(2, 3);
        // func3 = 0b000
        ctx.words.push_back(w);
    });
    add("CLEAR.P", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("CLEAR.P no operands");
        uint16_t w = makeBase(2, 3);
        w |= (1)<<13; // func3 = 0b001
        ctx.words.push_back(w);
    });
    add("SETRID.P", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SETRID.P pid");
        int pid=parseInt(ops[0]);
        if(pid<0||pid>31) throw AsmError("pid range");
        uint16_t w = makeBase(2, 2); // opcode=10 funct2=10
        setFunc3Func1(w, 1, 0); // func3=001 func1=0
        w |= (pid & 0x1F)<<5;
        ctx.words.push_back(w);
    });
    add("SETRID.T", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SETRID.T vtid");
        int vt=parseInt(ops[0]); if(vt<0||vt>3) throw AsmError("vtid range 0..3");
        uint16_t w = makeBase(2, 2); // opcode=10 funct2=10
        setFunc3Func1(w, 2, 0); // func3=010 func1=0
        w |= (vt & 0x3)<<10;
        ctx.words.push_back(w);
    });
    add("SETRID.PT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=2) throw AsmError("SETRID.PT pid, vtid");
        int pid=parseInt(ops[0]);
        int vt=parseInt(ops[1]);
        if(pid<0||pid>31) throw AsmError("pid range");
        if(vt<0||vt>3) throw AsmError("vtid range");
        uint16_t w = makeBase(2, 2); // opcode=10 funct2=10
        setFunc3Func1(w, 3, 0); // func3=011 func1=0
        w |= (pid & 0x1F) << 5;
        w |= (vt & 0x3) << 10;
        ctx.words.push_back(w);
    });
}

/*
    Parse template header line: template_name(param1=default1, param2, ...)
    Returns true if successfully parsed, fills in templateName and params
*/
bool Assembler::parseTemplateHeader(const std::string &line, std::string &templateName, std::vector<TemplateParam> &params) {
    // Format: template_name(param1=default1, param2=default2, ...):
    size_t openParen = line.find('(');
    size_t closeParen = line.rfind(')');
    size_t colon = line.rfind(':');

    if (openParen == std::string::npos || closeParen == std::string::npos || colon == std::string::npos) {
        return false;
    }

    templateName = trim(line.substr(0, openParen));
    std::string paramsStr = trim(line.substr(openParen + 1, closeParen - openParen - 1));

    if (paramsStr.empty()) {
        return true; // No parameters
    }

    // Parse parameters
    std::vector<std::string> paramTokens = splitOperands(paramsStr);
    for (const auto &token : paramTokens) {
        size_t eqPos = token.find('=');
        if (eqPos != std::string::npos) {
            // Has default value
            std::string name = trim(token.substr(0, eqPos));
            std::string valueStr = trim(token.substr(eqPos + 1));
            int value = parseInt(valueStr);
            params.push_back(TemplateParam(name, value, true));
        } else {
            // No default value, default to 0
            std::string name = trim(token);
            params.push_back(TemplateParam(name, 0, false));
        }
    }

    return true;
}

/*
    Extract template parameter from operand string like $(PARAM_NAME)
    Returns the parameter index if found, std::nullopt otherwise
*/
std::optional<int> Assembler::extractTemplateParam(const std::string &operand) {
    if (operand.size() < 4 || operand[0] != '$' || operand[1] != '(' || operand.back() != ')') {
        return std::nullopt;
    }

    std::string paramName = operand.substr(2, operand.size() - 3);
    auto it = templateParams.find(paramName);
    if (it == templateParams.end()) {
        throw AsmError("Undefined template parameter: " + paramName);
    }

    return it->second;
}

/*
    Assemble template code
    Returns TemplateResult with parameter definitions, patches, and instructions
*/
TemplateResult Assembler::assembleTemplate(const std::string &source, bool verbose) {
    if (verbose) std::cout << "Assembling template code...\n";

    TemplateResult result;
    result.isTemplate = false;

    EncoderCtx ctx;
    std::istringstream iss(source);
    std::string line;
    int lineno = 0;
    int instrIndex = 0;

    labels.clear();
    patches.clear();
    templateParams.clear();
    templatePatches.clear();

    bool inTemplate = false;

    while (std::getline(iss, line)) {
        ++lineno;
        auto posHash = line.find('#');
        if (posHash != std::string::npos) line = line.substr(0, posHash);
        line = trim(line);
        if (line.empty()) continue;

        // Check for .template directive
        if (line == ".template") {
            inTemplate = true;
            result.isTemplate = true;
            if (verbose) std::cout << "Found .template directive\n";
            continue;
        }

        // Parse template header
        if (inTemplate && result.name.empty()) {
            if (parseTemplateHeader(line, result.name, result.params)) {
                // Build parameter map
                for (size_t i = 0; i < result.params.size(); ++i) {
                    templateParams[result.params[i].name] = i;
                }
                if (verbose) {
                    std::cout << "Template: " << result.name << " with " << result.params.size() << " parameters:\n";
                    for (const auto &p : result.params) {
                        std::cout << "  - " << p.name;
                        if (p.hasDefault) std::cout << " = " << p.defaultValue;
                        std::cout << "\n";
                    }
                }
                continue;
            }
        }

        // Handle labels
        if (line.back() == ':') {
            std::string label = trim(line.substr(0, line.size() - 1));
            if (labels.count(label)) throw AsmError("Duplicate label: " + label);
            labels[label] = instrIndex;
            if (verbose) std::cout << "Label '" << label << "' at index " << instrIndex << "\n";
            continue;
        }

        // Parse instruction
        std::string mn;
        std::string rest;
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) {
            mn = line;
        } else {
            mn = line.substr(0, sp);
            rest = trim(line.substr(sp + 1));
        }

        std::string u = mn;
        std::transform(u.begin(), u.end(), u.begin(), ::toupper);
        auto it = table.find(u);
        if (it == table.end()) throw AsmError("Unknown mnemonic '" + mn + "' at line " + std::to_string(lineno));

        auto operands = splitOperands(rest);

        // Check for template parameters in operands
        // (only for LDMA.ADDR, LDMA.LEN, LOOPIN, LDMA.LOOP, SDMA.LOOP, SDMA.LEN, SDMA.ADDR)
        if (inTemplate && (u == "LDMA.ADDR" || u == "LDMA.LEN" || u == "LOOPIN" || u == "LDMA.LOOP" || u == "SDMA.LOOP" || u == "SDMA.LEN" || u == "SDMA.ADDR")) {
            if (operands.size() == 1) {
                auto paramIdx = extractTemplateParam(operands[0]);
                if (paramIdx.has_value()) {
                    // Record patch location
                    templatePatches.push_back(TemplatePatch(instrIndex, paramIdx.value()));
                    // Use default value or 0 for now
                    int defaultVal = result.params[paramIdx.value()].hasDefault
                                   ? result.params[paramIdx.value()].defaultValue
                                   : 0;
                    operands[0] = std::to_string(defaultVal);
                    if (verbose) {
                        std::cout << "  Template param at instruction " << instrIndex
                                  << ", param index " << paramIdx.value() << "\n";
                    }
                }
            }
        }

        try {
            it->second.fn(operands, ctx);
        } catch (const AsmError &e) {
            throw AsmError(std::string("Line ") + std::to_string(lineno) + ": " + e.what());
        }

        instrIndex = ctx.words.size();
        if (verbose) std::cout << "Instruction [" << instrIndex - 1 << "] '" << mn << " " << rest << "'\n";
    }

    // Apply label patches
    applyPatches(ctx.words);

    result.instructions = ctx.words;
    result.patches = templatePatches;

    if (verbose) {
        std::cout << "Template compilation completed.\n";
        std::cout << "  Instructions: " << result.instructions.size() << "\n";
        std::cout << "  Patches: " << result.patches.size() << "\n";
    }

    return result;
}

/*
    將操作數字串分割為單獨的 token
    支援逗號分隔和空格分隔
    返回一個字符串向量
*/
void Assembler::applyPatches(std::vector<uint16_t> &words){
    for(auto &p: patches){
        if(p.type==Patch::JUMP){
            auto it = labels.find(p.label);
            if(it==labels.end()) throw AsmError("Undefined label: "+p.label);
            int targetIdx = it->second; // 指令 index
            int byteAddr = targetIdx * 2; // J 使用 byte address, 2-byte 對齊
            if(byteAddr<0 || byteAddr>0x7FF) throw AsmError("Label target out of range: "+p.label);
            uint16_t w = makeBase(1, 2); // opcode=01 funct2=10
            int b_9_7 = (byteAddr >> 7) & 0x7;
            int b_10 = (byteAddr >> 10) &1;
            int b_0 = byteAddr &1;
            int b_6_1 = (byteAddr>>1)&0x3F;
            w |= b_9_7<<13;
            w |= b_10<<12;
            w |= b_0<<11;
            w |= b_6_1<<5;
            w |= (words[p.index] & 1); // 保留 loopEnd
            words[p.index] = w;
        }
    }
}
/*
    組譯器主函式
    讀取源碼，處理標籤和指令，並返回組譯後的指令集 (vector<uint16_t>)

    1. 第一遍讀取源碼，處理標籤和指令，記錄指令位置。
    2. 第二遍處理跳轉標籤，將標籤位置填入指令中。

    異常情況會拋出 AsmError。
*/
std::vector<uint16_t> Assembler::assemble(const std::string &source, bool verbose){
    if(verbose) std::cout << "Assembling source code...\n";
    EncoderCtx ctx;
    std::istringstream iss(source);
    std::string line;
    int lineno=0;
    int instrIndex=0;
    labels.clear();
    patches.clear();
    // first pass: 處理標籤和指令
    while(std::getline(iss,line)){
        ++lineno;
        auto posHash = line.find('#'); // 註解符號
        if(posHash!=std::string::npos) line = line.substr(0,posHash); // 去除註解
        line = trim(line);
        if(line.empty()) continue;
        if(line.back()==':'){ // 標籤
            std::string label = trim(line.substr(0,line.size()-1));
            if(labels.count(label)) throw AsmError("Duplicate label: "+label);
            labels[label]=instrIndex; continue; // 記錄標籤位置
            if(verbose) std::cout << "Label '" << label << "' at index " << instrIndex << "\n";
        }
        std::string mn;
        std::string rest;
        size_t sp = line.find_first_of(" \t"); // 找到第一個空白或制表符
        if(sp==std::string::npos){
            mn=line;
        } else {
            mn = line.substr(0,sp); // 指令名稱
            rest = trim(line.substr(sp+1)); // 剩餘部分
        }
        std::string u=mn;
        std::transform(u.begin(),u.end(),u.begin(),::toupper); // 將指令名稱轉為大寫
        auto it = table.find(u); // 在對應表中查找
        if(it==table.end()) throw AsmError("Unknown mnemonic '"+mn+"' at line "+std::to_string(lineno)); // 未知指令
        auto operands = splitOperands(rest); // 分割操作數
        try {
            it->second.fn(operands, ctx);
        } catch(const AsmError &e){
            throw AsmError(std::string("Line ")+std::to_string(lineno)+": "+e.what());
        }
        instrIndex = ctx.words.size();
        if(verbose) std::cout << "Instruction ["<< instrIndex-1<<"] '" << mn << " " << rest << "'\n";
    }
    // second pass: 處理跳轉標籤
    applyPatches(ctx.words);
    if(verbose) std::cout << "Assembling completed. Total instructions: " << ctx.words.size() << "\n";
    return ctx.words;
}


/*
    將 16-bit 指令解碼為可讀的字符串表示
    支援 HALT/NOP/DMA/VMAC 等指令
    返回解碼後的字符串
*/
std::string Disassembler::disasmWord(uint16_t w) const {
    std::ostringstream os;
    bool loopEnd = w & 1;
    int opcode = (w>>1)&0x3;
    int funct2=(w>>3)&0x3;
    int func3=(w>>13)&0x7;
    int func1=(w>>12)&1;
    int payload=(w>>5)&0x7F;
    if(opcode==3 && funct2==3){
        if(func3==4) os<<"SWAPDM";
        else os<<"HALT";
    }
    else if(opcode==2 && funct2==0){ os<<"NOP"; }
    else if(opcode==0 && funct2==0){
        int bits6_1 = payload & 0x3F;
        int bit0   = (payload >> 6) & 0x1;
        int val = (func3 << 7) | (bits6_1 << 1) | bit0;
        if(func1==0) os<<"SDMA.ADDR "<<val; else os<<"SDMA.LEN "<<val;
    }
    else if(opcode==0 && funct2==1){
        int bits6_1 = payload & 0x3F;           // payload bits5:0 -> [10:5] => value[6:1]
        int bit0   = (payload >> 6) & 0x1;      // payload bit6   -> [11]   => value[0]
        int val = (func3 << 7) | (bits6_1 << 1) | bit0; // 10-bit
        if(func1==0) os<<"LDMA.ADDR "<<val; else os<<"LDMA.LEN "<<val;
    }
    else if(opcode==0 && funct2==2){
        static const char* names[7] = {"LDMA.LB","LDMA.LH","LDMA.LW","LDMA.LD","LDMA.LBB","LDMA.LHB","LDMA.LWB"};
        if(func3<=6){ os<<names[func3]<<" "<< ((w>>10)&0x7); }
        else os<<"DMA.?";
    }
    else if(opcode==0 && funct2==3 && func3==3){ os<<"SDMA.SD "<< ((w>>10)&0x7); }
    else if(opcode==2 && funct2==1){
        switch(func3){
            case 0: os<<(func1?"VMACN":"VMAC")<<" P"<<((w>>5)&0x1F)<<", VT"<<((w>>10)&0x3); break;
            case 1: os<<(func1?"VMACRN":"VMACR")<<" PSTR="<<((w>>5)&0x1F)<<", VTSTR="<<((w>>10)&0x3); break;
            case 2: os<<(func1?"VMULN":"VMUL")<<" P"<<((w>>5)&0x1F)<<", VT"<<((w>>10)&0x3); break;
            case 3: os<<(func1?"VMULRN":"VMULR")<<" VPSTR="<<((w>>5)&0x1F)<<", VTSTR="<<((w>>10)&0x3); break;
            case 4: os<<"VPSUM VPRS=P"<<((w>>5)&0x1F); break;
            case 5: os<<"VPSUMR VPSTR="<<((w>>5)&0x1F); break;
            default: os<<"ARITH?"; break;
        }
    }
    else if(opcode==2 && funct2==2){ // SETRID group
        switch(func3){
            case 1: os<<"SETRID.P "<<((w>>5)&0x1F); break;
            case 2: os<<"SETRID.T "<<((w>>10)&0x3); break;
            case 3: os<<"SETRID.PT "<<((w>>5)&0x1F)<<", "<<((w>>10)&0x3); break;
            default: os<<"RID?"; break;
        }
    }
    else if(opcode==2 && funct2==3){ // CLEAR group
        switch(func3){
            case 0: os<<"CLEAR.T"; break; case 1: os<<"CLEAR.P"; break; default: os<<"CLEAR?"; break;
        }
    }
    else if(opcode==1 && funct2==1){
        int bits6_1 = payload & 0x3F;
        int bit0   = (payload >> 6) & 0x1;
        int val = (func3 << 7) | (bits6_1 << 1) | bit0;
        val += 1; // 0-based decoding
        if(func1==0) os<<"LDMA.LOOP "<<val; else os<<"SDMA.LOOP "<<val;
    }
    else if(opcode==1 && funct2==2){
        int imm = ((func3 & 0x7)<<7) | (func1<<10) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
        os<<"J 0x"<<std::hex<<imm<<std::dec<<" (idx="<<(imm/2)<<")";
    }
    else if(opcode==1 && funct2==3){
        if(func1==0){ int lc = ((func3 &0x7)<<7) | (((w>>11)&1)) | ((payload & 0x3F)<<1); lc+=1; os<<"LOOPIN "<<lc; }
        else os<<"LOOPBREAK";
    }
    else if(opcode==1 && funct2==0){ // TSTORE / TSHIFT 新規格 funct2=00
        if(func3==0){ // TSTORE
            int trd = (w>>5)&0xf; os<<"TSTORE "<<trd; }
        else if(func3==1){ // TSHIFT
            int code = (w>>10)&0x7; int k = (code==0?3:(code==1?5:(code==2?7:-1)));
            if(k==-1) os<<"TSHIFT ?"; else os<<"TSHIFT "<<k;
        } else {
            os<<"T?";
        }
    }
    else { os<<".word 0x"<<std::hex<<w; }
    if(loopEnd) os<<" ; LOOPEND"; return os.str();
}

} // namespace hybridacc
