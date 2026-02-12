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
        if(ctx.words.back() & 0x1) ctx.words.push_back(0x14|0x1); // NOP with LOOPEND
        else ctx.words.back() |= 0x1; // loop-end flag
    });

    // LDMA.ADDR start_addr (opcode=00 func2=00 func1=0)
    add("LDMA.ADDR", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LDMA.ADDR expects 1 operand");
        int addr = parseInt(ops[0]);
        if(addr < 0 || addr > 0x3FF) throw AsmError("start_addr out of range (0..1023)");
        uint16_t w = makeBase(0, 0, 0);
        setPayload(w, addr);
        ctx.words.push_back(w);
    });

    // SDMA.ADDR start_addr (opcode=00 func2=00 func1=1)
    add("SDMA.ADDR", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.ADDR expects 1 operand");
        int addr = parseInt(ops[0]);
        if(addr < 0 || addr > 0x3FF) throw AsmError("start_addr out of range (0..1023)");
        uint16_t w = makeBase(0, 0, 1);
        setPayload(w, addr);
        ctx.words.push_back(w);
    });

    // LDMA.LEN len (opcode=00 func2=01 func1=0)
    add("LDMA.LEN", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LDMA.LEN expects 1 operand");
        int len = parseInt(ops[0]) - 1;
        if(len < 0 || len > 0x3FF) throw AsmError("len out of range (1..1024)");
        uint16_t w = makeBase(0, 1, 0);
        setPayload(w, len);
        ctx.words.push_back(w);
    });

    // SDMA.LEN len (opcode=00 func2=01 func1=1)
    add("SDMA.LEN", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.LEN expects 1 operand");
        int len = parseInt(ops[0]) - 1;
        if(len < 0 || len > 0x3FF) throw AsmError("len out of range (1..1024)");
        uint16_t w = makeBase(0, 1, 1);
        setPayload(w, len);
        ctx.words.push_back(w);
    });

    // LDMA.LOOP count (opcode=00 func2=10 func1=0)
    add("LDMA.LOOP", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LDMA.LOOP expects 1 operand");
        int count = parseInt(ops[0]) - 1;
        if(count < 0 || count > 0x3FF) throw AsmError("count out of range (1..1024)");
        uint16_t w = makeBase(0, 2, 0);
        setPayload(w, count);
        ctx.words.push_back(w);
    });

    // SDMA.LOOP count (opcode=00 func2=10 func1=1)
    add("SDMA.LOOP", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.LOOP expects 1 operand");
        int count = parseInt(ops[0]) - 1;
        if(count < 0 || count > 0x3FF) throw AsmError("count out of range (1..1024)");
        uint16_t w = makeBase(0, 2, 1);
        setPayload(w, count);
        ctx.words.push_back(w);
    });

    auto addDMALoad = [&](const std::string &mn, int func3){
        add(mn, [=](const std::vector<std::string>&ops, EncoderCtx &ctx){
            if(ops.size()!=1) throw AsmError(mn+" expects 1 operand stride");
            int stride = parseInt(ops[0]);
            if(stride<0 || stride>7) throw AsmError("stride out of range(0..7)");
            uint16_t w = makeBase(0, 3, 0); // opcode=00 func2=11 func1=0
            int payload = ((stride & 0x7) << 3) | (func3 & 0x7);
            setPayload(w, payload);
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

    // SDMA.SD stride (store double-word) opcode=00 func2=11 func1=0 func3=111
    add("SDMA.SD", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SDMA.SD expects 1 operand");
        int stride = parseInt(ops[0]); if(stride<0||stride>7) throw AsmError("stride out of range");
        uint16_t w = makeBase(0, 3, 0);
        int payload = ((stride & 0x7) << 3) | 0x7;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });

    // TSTORE trd (opcode=00 func2=11 func1=1, payload[15:12]=trd, payload[8:6]=000)
    add("TSTORE", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("TSTORE expects 1 operand (trd)");
        int trd = parseRegIndex(ops[0]);
        uint16_t w = makeBase(0, 3, 1);
        int payload = (trd & 0xF) << 6;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });

    // VTSTORE vtrd (opcode=00 func2=11 func1=1, payload[10:9]=vtrd, payload[8:6]=001)
    add("VTSTORE", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("VTSTORE expects 1 operand (vtrd)");
        int vtrd = parseRegIndex(ops[0]);
        if(vtrd<0 || vtrd>2) throw AsmError("vtrd out of range (0..2)");
        uint16_t w = makeBase(0, 3, 1);
        int payload = ((vtrd & 0x3) << 3) | 0x1;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });

    // TSHIFT (opcode=00 func2=11 func1=1, payload[11:9]=kernel_size, payload[8:6]=010)
    add("TSHIFT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("TSHIFT expects kernel_size (K3/K5/K7 or 3/5/7)");
        int k;
        if(!parseSpecialToken(ops[0], k)) {
            try { k = parseInt(ops[0]); } catch(...) { throw AsmError("Invalid kernel size token"); }
        }
        int encoded_k;
        switch(k){
             case 3: encoded_k = 0; break;
             case 5: encoded_k = 1; break;
             case 7: encoded_k = 2; break;
             default: throw AsmError("Unsupported kernel size (3/5/7)");
        }
        uint16_t w = makeBase(0, 3, 1);
        int payload = ((encoded_k & 0x3) << 3) | 0x2;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });

    auto addVMx = [&](const std::string &mn, int func2, int func3, bool hasN, bool regCtrl){
        add(mn, [=](const std::vector<std::string>&ops, EncoderCtx &ctx){
            int func1 = 0;
            if(hasN && mn.back()=='N') func1=1; // 尾碼 N
            uint16_t w = makeBase(1, func2, func1); // opcode=01
            int payload = 0;
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
                payload = ((prd & 0x1F) << 5) | ((vtrs & 0x3) << 3) | (func3 & 0x7);
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
                if(vts<0||vts>3) throw AsmError("vtstride out of range (0..3)");
                payload = ((ps & 0x1F) << 5) | ((vts & 0x3) << 3) | (func3 & 0x7);
            }
            setPayload(w, payload);
            ctx.words.push_back(w);
        });
    };
    addVMx("VMAC",  0, 0, true, false);
    addVMx("VMACN", 0, 0, true, false);
    addVMx("VMACR", 0, 1, true, true);
    addVMx("VMACRN",0, 1, true, true);
    addVMx("VMUL",  1, 0, true, false);
    addVMx("VMULN", 1, 0, true, false);
    addVMx("VMULR", 1, 1, true, true);
    addVMx("VMULRN",1, 1, true, true);

    add("VPSUM", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("VPSUM expects vprs");
        int vprs = parseRegIndex(ops[0]);
        if(vprs<0||vprs>31) throw AsmError("vprs out of range");
        uint16_t w = makeBase(1, 2, 0); // opcode=01 func2=10
        int payload = ((vprs & 0x1F) << 5) | 0x0;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });
    add("VPSUMR", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("VPSUMR expects vpstride");
        int vpstride = parseInt(ops[0]);
        if(vpstride<0||vpstride>31) throw AsmError("vpstride out of range");
        uint16_t w = makeBase(1, 2, 0); // opcode=01 func2=10
        int payload = ((vpstride & 0x1F) << 5) | 0x1;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });
    // Mixed control
    add("VPSUM_VTSTORE", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=2) throw AsmError("VPSUM_VTSTORE expects vprs and vtrd");
        int vprs = parseRegIndex(ops[0]);
        if(vprs<0||vprs>31) throw AsmError("vprs out of range");
        int vtrd = parseRegIndex(ops[1]);
        if(vtrd<0||vtrd>2) throw AsmError("vtrd out of range (0..2)");
        uint16_t w = makeBase(1, 3, 0); // opcode=01 func2=11
        int payload = ((vprs & 0x1F) << 5) | ((vtrd & 0x3) << 3) | 0x0;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });
    add("VPSUMR_VTSTORE", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=2) throw AsmError("VPSUMR_VTSTORE expects vpstride and vtrd");
        int vpstride = parseRegIndex(ops[0]);
        if(vpstride<0||vpstride>31) throw AsmError("vpstride out of range");
        int vtrd = parseRegIndex(ops[1]);
        if(vtrd<0||vtrd>2) throw AsmError("vtrd out of range (0..2)");
        uint16_t w = makeBase(1, 3, 0); // opcode=01 func2=11
        int payload = ((vpstride & 0x1F) << 5) | ((vtrd & 0x3) << 3) | 0x1;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });
    add("VPSUM_TSHIFT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=2) throw AsmError("VPSUM_TSHIFT expects vprs and kernel_size");
        int vprs = parseRegIndex(ops[0]);
        if(vprs<0||vprs>31) throw AsmError("vprs out of range");
        int kernel_size = parseRegIndex(ops[1]);
        int encoded_k;
        switch(kernel_size){
             case 3: encoded_k = 0; break;
             case 5: encoded_k = 1; break;
             case 7: encoded_k = 2; break;
             default: throw AsmError("Unsupported kernel size (3/5/7)");
        }
        uint16_t w = makeBase(1, 3, 0); // opcode=01 func2=11
        int payload = ((vprs & 0x1F) << 5) | ((encoded_k & 0x3) << 3) | 0x2;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });
    add("VPSUMR_TSHIFT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=2) throw AsmError("VPSUMR_TSHIFT expects vpstride and kernel_size");
        int vpstride = parseRegIndex(ops[0]);
        if(vpstride<0||vpstride>31) throw AsmError("vpstride out of range");
        int kernel_size = parseRegIndex(ops[1]);
        int encoded_k;
        switch(kernel_size){
             case 3: encoded_k = 0; break;
             case 5: encoded_k = 1; break;
             case 7: encoded_k = 2; break;
             default: throw AsmError("Unsupported kernel size (3/5/7)");
        }
        uint16_t w = makeBase(1, 3, 0); // opcode=01 func2=11
        int payload = ((vpstride & 0x1F) << 5) | ((encoded_k & 0x3) << 3) | 0x3;
        setPayload(w, payload);
        ctx.words.push_back(w);
    });


    // LOOPIN / LOOPBREAK (opcode=10 func2=00)
    add("LOOPIN", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("LOOPIN expects loop_count");
        int lc = parseInt(ops[0]) - 1;
        if(lc < 0 || lc > 0x3FF) throw AsmError("loop_count out of range (1..1024)");
        uint16_t w = makeBase(2, 0, 0);
        setPayload(w, lc);
        ctx.words.push_back(w);
    });

    // System level (opcode=10)
    auto emitSysCtrl = [&](EncoderCtx &ctx, int payload){
        uint16_t w = makeBase(2, 1, 0);
        setPayload(w, payload);
        ctx.words.push_back(w);
    };
    auto emitSysSync = [&](EncoderCtx &ctx, int payload){
        uint16_t w = makeBase(2, 1, 1);
        setPayload(w, payload);
        ctx.words.push_back(w);
    };

    add("SYS.CTRL", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.size()!=1) throw AsmError("SYS.CTRL expects flag list");
        std::string s = trim(ops[0]);
        if(!s.empty() && s.front()=='(' && s.back()==')') s = trim(s.substr(1, s.size()-2));
        auto flags = splitOperands(s);
        bool hasSwap = false;
        int payload = 0;
        for(auto &f : flags){
            std::string u = trim(f);
            if(u.empty()) continue;
            std::transform(u.begin(),u.end(),u.begin(),::toupper);
            if(u=="SWAPDM") { hasSwap = true; continue; }
            if(u=="SDMA.ACT") payload |= (1<<7);
            else if(u=="SDMA.RST") payload |= (1<<6);
            else if(u=="LDMA.ACT") payload |= (1<<5);
            else if(u=="LDMA.RST") payload |= (1<<4);
            else if(u=="RST.PID") payload |= (1<<3);
            else if(u=="RST.TID") payload |= (1<<2);
            else if(u=="CLEAR.T") payload |= (1<<1);
            else if(u=="CLEAR.P") payload |= (1<<0);
            else throw AsmError("Unknown SYS.CTRL flag: "+u);
        }
        if(hasSwap){
            if(payload!=0) throw AsmError("SWAPDM cannot be combined with other SYS.CTRL flags");
            emitSysSync(ctx, 1);
        } else {
            emitSysCtrl(ctx, payload);
        }
    });

    add("SYS.SYNC", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(ops.empty()) { emitSysSync(ctx, 0); return; }
        if(ops.size()!=1) throw AsmError("SYS.SYNC expects optional SWAPDM flag");
        std::string s = trim(ops[0]);
        if(!s.empty() && s.front()=='(' && s.back()==')') s = trim(s.substr(1, s.size()-2));
        std::transform(s.begin(),s.end(),s.begin(),::toupper);
        if(s.empty() || s=="SWAPDM") emitSysSync(ctx, 1); else throw AsmError("SYS.SYNC only supports SWAPDM");
    });

    add("NOP", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("NOP no operands");
        uint16_t w = makeBase(2, 2, 0);
        setPayload(w, 0);
        ctx.words.push_back(w);
    });

    add("HALT", [&](const std::vector<std::string>&ops, EncoderCtx &ctx){
        if(!ops.empty()) throw AsmError("HALT no operands");
        uint16_t w = makeBase(2, 3, 0);
        setPayload(w, 0);
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
    auto isDmaUseMnemonic = [&](const std::string &u){
        return (u == "VMACN" || u == "VMACRN" || u == "VMULN" || u == "VMULRN");
    };
    auto hasLdmaActFlag = [&](const std::string &rest){
        std::string s = trim(rest);
        if (s.empty()) return false;
        if (!s.empty() && s.front() == '(' && s.back() == ')') {
            s = trim(s.substr(1, s.size() - 2));
        }
        auto flags = splitOperands(s);
        for (auto &f : flags) {
            std::string u = trim(f);
            if (u.empty()) continue;
            std::transform(u.begin(), u.end(), u.begin(), ::toupper);
            if (u == "LDMA.ACT") return true;
        }
        return false;
    };
    auto emitNop = [&](EncoderCtx &ctxRef){
        uint16_t w = makeBase(2, 2, 0);
        setPayload(w, 0);
        ctxRef.words.push_back(w);
    };
    std::istringstream iss(source);
    std::string line;
    int lineno = 0;
    int instrIndex = 0;

    labels.clear();
    patches.clear();
    templateParams.clear();
    templatePatches.clear();

    bool inTemplate = false;
    bool pendingLdmaAct = false;

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

        if (pendingLdmaAct) {
            if (isDmaUseMnemonic(u)) {
                emitNop(ctx);
                if (verbose) std::cout << "Inserted NOP after LDMA.ACT before '" << u << "'\n";
            }
            pendingLdmaAct = false;
        }

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

        if (u == "SYS.CTRL" && hasLdmaActFlag(rest)) {
            pendingLdmaAct = true;
        }

        instrIndex = ctx.words.size();
        if (verbose) std::cout << "Instruction [" << instrIndex - 1 << "] '" << mn << " " << rest << "'\n";
    }

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
    組譯器主函式
    讀取源碼，處理標籤和指令，並返回組譯後的指令集 (vector<uint16_t>)

    1. 第一遍讀取源碼，處理標籤和指令，記錄指令位置。
    2. 第二遍處理跳轉標籤，將標籤位置填入指令中。

    異常情況會拋出 AsmError。
*/
std::vector<uint16_t> Assembler::assemble(const std::string &source, bool verbose){
    if(verbose) std::cout << "Assembling source code...\n";
    EncoderCtx ctx;
    auto isDmaUseMnemonic = [&](const std::string &u){
        return (u == "VMACN" || u == "VMACRN" || u == "VMULN" || u == "VMULRN");
    };
    auto hasLdmaActFlag = [&](const std::string &rest){
        std::string s = trim(rest);
        if (s.empty()) return false;
        if (!s.empty() && s.front() == '(' && s.back() == ')') {
            s = trim(s.substr(1, s.size() - 2));
        }
        auto flags = splitOperands(s);
        for (auto &f : flags) {
            std::string u = trim(f);
            if (u.empty()) continue;
            std::transform(u.begin(), u.end(), u.begin(), ::toupper);
            if (u == "LDMA.ACT") return true;
        }
        return false;
    };
    auto emitNop = [&](EncoderCtx &ctxRef){
        uint16_t w = makeBase(2, 2, 0);
        setPayload(w, 0);
        ctxRef.words.push_back(w);
    };
    std::istringstream iss(source);
    std::string line;
    int lineno=0;
    int instrIndex=0;
    bool pendingLdmaAct = false;
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

        if (pendingLdmaAct) {
            if (isDmaUseMnemonic(u)) {
                emitNop(ctx);
                if (verbose) std::cout << "Inserted NOP after LDMA.ACT before '" << u << "'\n";
            }
            pendingLdmaAct = false;
        }

        auto operands = splitOperands(rest); // 分割操作數
        try {
            it->second.fn(operands, ctx);
        } catch(const AsmError &e){
            throw AsmError(std::string("Line ")+std::to_string(lineno)+": "+e.what());
        }

        if (u == "SYS.CTRL" && hasLdmaActFlag(rest)) {
            pendingLdmaAct = true;
        }
        instrIndex = ctx.words.size();
        if(verbose) std::cout << "Instruction ["<< instrIndex-1<<"] '" << mn << " " << rest << "'\n";
    }

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
    int func2=(w>>3)&0x3;
    int func1=(w>>5)&0x1;
    int payload=(w>>6)&0x3FF;
    int func3 = payload & 0x7;
    int reg5 = (payload>>5) & 0x1F;
    int vtbits = (payload>>3) & 0x3;
    int stride = (payload>>3) & 0x7;

    if(opcode==2 && func2==3 && func1==0){
        os<<"HALT";
    }
    else if(opcode==2 && func2==2 && func1==0){
        os<<"NOP";
    }
    else if(opcode==2 && func2==1){
        if(func1==1){
            if(payload & 0x1) os<<"SYS.SYNC (SWAPDM)"; else os<<"SYS.SYNC";
        } else {
            std::vector<std::string> flags;
            if(payload & (1<<7)) flags.push_back("SDMA.ACT");
            if(payload & (1<<6)) flags.push_back("SDMA.RST");
            if(payload & (1<<5)) flags.push_back("LDMA.ACT");
            if(payload & (1<<4)) flags.push_back("LDMA.RST");
            if(payload & (1<<3)) flags.push_back("RST.PID");
            if(payload & (1<<2)) flags.push_back("RST.TID");
            if(payload & (1<<1)) flags.push_back("CLEAR.T");
            if(payload & (1<<0)) flags.push_back("CLEAR.P");
            os<<"SYSCTRL (";
            for(size_t i=0;i<flags.size();++i){ if(i) os<<", "; os<<flags[i]; }
            os<<")";
        }
    }
    else if(opcode==2 && func2==0 && func1==0){
        os<<"LOOPIN "<<payload+1;
    }
    else if(opcode==0 && func2==0){
        if(func1==0) os<<"LDMA.ADDR "<<payload;
        else os<<"SDMA.ADDR "<<payload;
    }
    else if(opcode==0 && func2==1){
        if(func1==0) os<<"LDMA.LEN "<<payload+1;
        else os<<"SDMA.LEN "<<payload+1;
    }
    else if(opcode==0 && func2==2){
        if(func1==0) os<<"LDMA.LOOP "<<payload+1;
        else os<<"SDMA.LOOP "<<payload+1;
    }
    else if(opcode==0 && func2==3){
        if(func1==0){
            static const char* names[7] = {"LDMA.LB","LDMA.LH","LDMA.LW","LDMA.LD","LDMA.LBB","LDMA.LHB","LDMA.LWB"};
            if(func3==7) os<<"SDMA.SD "<<stride;
            else if(func3<=6) os<<names[func3]<<" "<<stride;
            else os<<"DMA.?";
        } else {
            if(func3==0){
                int trd = (payload>>6)&0xF; os<<"TSTORE T"<<trd;
            } else if(func3==1){
                int vtrd = (payload>>3)&0x3; os<<"VTSTORE VT"<<vtrd;
            } else if(func3==2){
                int encoded_k = (payload>>3)&0x3;
                switch (encoded_k) {
                    case 0: os<<"TSHIFT K3"; break;
                    case 1: os<<"TSHIFT K5"; break;
                    case 2: os<<"TSHIFT K7"; break;
                    default: os<<"TSHIFT K?";
                }
            } else {
                os<<"T?";
            }
        }
    }
    else if(opcode==1){
        if(func2==0){
            if(func3==0){ os<<(func1?"VMACN":"VMAC")<<" P"<<reg5<<", VT"<<vtbits; }
            else if(func3==1){ os<<(func1?"VMACRN":"VMACR")<<" "<<reg5<<", "<<(vtbits==3?"VTRST":("VT"+std::to_string(vtbits))); }
            else os<<"ARITH?";
        } else if(func2==1){
            if(func3==0){ os<<(func1?"VMULN":"VMUL")<<" VP"<<reg5<<", VT"<<vtbits; }
            else if(func3==1){ os<<(func1?"VMULRN":"VMULR")<<" "<<reg5<<", "<<(vtbits==3?"VTRST":("VT"+std::to_string(vtbits))); }
            else os<<"ARITH?";
        } else if(func2==2){
            if(func3==0) os<<"VPSUM VP"<<reg5;
            else if(func3==1) os<<"VPSUMR "<<reg5;
            else os<<"ARITH?";
        } else if(func2==3){
            if(func3==0) os<<"VPSUM_VTSTORE VP"<<reg5<<", VT"<<vtbits;
            else if(func3==1) os<<"VPSUMR_VTSTORE "<<reg5<<", VT"<<vtbits;
            else if(func3==2) os<<"VPSUM_TSHIFT VP"<<reg5<<", K"<<((payload>>3)&0x3)*2+3;
            else if(func3==3) os<<"VPSUMR_TSHIFT "<<reg5<<", K"<<((payload>>3)&0x3)*2+3;
        } else {
            os<<"ARITH?";
        }
    }
    else { os<<".word 0x"<<std::hex<<w; }
    if(loopEnd) os<<" ; LOOPEND"; return os.str();
}

} // namespace hybridacc
