#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <deque>
#include <stdexcept>
#include <functional>
#include <fstream>

#include "component.hpp"
#include "simulator.hpp"
#include "../assambler/instruction.hpp"
#include "../assambler/utils.hpp"

namespace hybridacc {

// Helper field extractors
static inline int getOpcode(uint16_t w){ return (w>>1) & 0x3; }
static inline int getFunct2(uint16_t w){ return (w>>3) & 0x3; }
static inline int getFunc3(uint16_t w){ return (w>>13)&0x7; }
static inline int getFunc1(uint16_t w){ return (w>>12)&0x1; }
static inline int getPayload(uint16_t w){ return (w>>5)&0x7F; }


bool PortIO::popPD(uint16_t &v) {
    if(PD.empty()) {
        std::cerr << "PD queue is empty" << std::endl;
        v = 0; // 返回 0 或其他預設值
        // return false;
        return true;
    }
    v = PD.front(); PD.pop();
    return true;
}
bool PortIO::popPS(uint64_t &v) {
    if(PS.empty()) {
        std::cerr << "PS queue is empty" << std::endl;
        v = 0;
        // return false;
        return true;
    }
    v = PS.front(); PS.pop();
    return true;
}
bool PortIO::popPIL(uint64_t &v) {
    if(PIL.empty()) {
        std::cerr << "PIL queue is empty" << std::endl;
        v = 0;
        // return false;
        return true;
    }
    v = PIL.front(); PIL.pop();
    return true;
}

std::vector<uint16_t> PortIO::getPD() const {
    std::vector<uint16_t> result;
    std::queue<uint16_t> temp = PD; // 複製 PD 佇列
    while (!temp.empty()) {
        result.push_back(temp.front());
        temp.pop();
    }
    return result;
}


// ----------------- 新增: PESimulator 基本介面實作 -----------------
PESimulator::PESimulator(const PEConfig &cfg){
    state.cfg = cfg;
    state.IM.resize(cfg.im_words);
    state.DM.resize(cfg.dm_words);
    state.DMA.init(&state.DM, &state.dmrv, &state.dmwv, cfg.dma_latency);
    state.TR.reset();
    state.PS.reset();
    state.pc = 0;
    state.halted = false;
    state.cycles = 0;
}

void PESimulator::loadProgram(const std::vector<uint16_t>& prog){
    state.IM.load(prog);
    state.pc = 0;
    state.halted = false;
    state.cycles = 0;
}

PEState& PESimulator::getState(){ return state; }

void PESimulator::run(uint64_t max_cycles){
    while(!state.halted && (max_cycles==0 || state.cycles < max_cycles)){
        step();
    }
}

// ----------------- 指令執行 -----------------
void PESimulator::execute(uint16_t w) {
    int opcode = getOpcode(w);
    int funct2 = getFunct2(w);
    int func3  = getFunc3(w);
    int func1  = getFunc1(w);
    int payload= getPayload(w);

    auto &S = state;

    // HALT
    if(opcode==3 && funct2==3){ S.halted = true; return; }
    // NOP
    if(opcode==2 && funct2==0){ return; }

    // DMA.ADDR / DMA.LEN (10-bit)
    if(opcode==0 && funct2==1){
        int bits6_1 = payload & 0x3F;
        int bit0 = (payload >> 6) & 0x1;
        int val = (func3<<7) | (bits6_1<<1) | bit0; // 10-bit
        if(func1==0) S.DMA.setBase(val); else S.DMA.setLen(val);
        return;
    }
    // DMA Loads / Broadcast Loads
    if(opcode==0 && funct2==2){
        int stride = (w>>10)&0x7;
        switch(func3){
            case 0: S.DMA.issue(DMARequestType::LOAD_BYTE, stride, false); break; // LB
            case 1: S.DMA.issue(DMARequestType::LOAD_HALF, stride, false); break; // LH
            case 2: S.DMA.issue(DMARequestType::LOAD_WORD, stride, false); break; // LW
            case 3: S.DMA.issue(DMARequestType::LOAD_DWORD, stride, false); break; // LD
            case 4: S.DMA.issue(DMARequestType::LOAD_BYTE, stride, true ); break; // LBB
            case 5: S.DMA.issue(DMARequestType::LOAD_HALF, stride, true ); break; // LHB
            case 6: S.DMA.issue(DMARequestType::LOAD_WORD, stride, true ); break; // LWB
            default: break;
        }
        return;
    }
    // DMA Stores (only SD simplified -> just advance base)
    if(opcode==0 && funct2==3){
        if(func3==3){
            S.DMA.issue(DMARequestType::STORE_DWORD, 1, false); // SD
            while (S.DMA.busy()) {
                uint64_t ps_word = 0;
                if (!port_io || !port_io->popPS(ps_word)) {
                    throw std::runtime_error("Blocking read failed: PS port is empty");
                }
                S.dmwv.fromUint64(ps_word);
                S.DMA.next();
            }
        }
        return;
    }

    // TSTORE / TSHIFT
    if(opcode==1 && funct2==0){
        if(func3==0){ // TSTORE trd
            int trd = (w>>5)&0xf; // bits 8:5
            uint16_t val = 0;
            port_io->popPD(val); // 從 PD 佇列讀取值
            state.TR.setT(trd, val);
        }
        else if(func3==1){ // TSHIFT k
            int code = (w>>10)&0x7; // kernel size code (0:K3 1:K5 2:K7)
            uint16_t maskBits = 0;
            switch(code){
                case 0: // K3 -> 110110110110b
                    maskBits = 0b110110110110; break;
                case 1: // K5 -> 111101111000b
                    maskBits = 0b111101111000; break;
                case 2: // K7 -> 111111000000b
                    maskBits = 0b111111000000; break;
                default: maskBits = 0; break; // 未定義 code 清空
            }
            state.TR.shift(maskBits); // 將 T registers 向右移動
        }
        return;
    }

    // Arithmetic Group
    if(opcode==2 && funct2==1){
        switch(func3){
            case 0: // VMAC / VMACN : P[prd] += dot(VT, DMRV)
            case 2: { // VMUL / VMULN : VP64[prd] = VT * DMRV + VP64[prd]
                int prd = (w>>5)&0x1F;
                int vtrs = (w>>10)&0x3;
                // 取得向量 VT
                Vector vt = state.TR.getVT(vtrs);
                if(func3==2){ // VMUL family => 向量輸出
                    Vector oldv = state.PS.getVP64(prd);
                    Vector newv = state.valu.vmul(vt, state.dmrv, oldv);
                    state.PS.setVP64(prd, newv);
                } else { // VMAC family => 累加到標量 P
                    Element oldp = state.PS.getP(prd);
                    Element newp = state.valu.vmac(vt, state.dmrv, oldp);
                    state.PS.setP(prd, newp);
                }
                if(func1){ S.DMA.next(); } // N 變形: 允許下一個 DMA
            } break;
            case 1: // VMACR / VMACRN : P[psum_cnt] += dot(VT[vtid_cnt], DMRV)
            case 3: { // VMULR / VMULRN : VP[vpsum_cnt] += mul(VT[vtid_cnt], DMRV)
                int pstride = (w>>5)&0x1F;
                int vpstride = (w>>5)&0x03;
                int vtstride = (w>>10)&0x3;
                // 取得向量 VT
                int prd = state.PS.psum_cnt; // 使用 psum_cnt 作為 P 索引
                int vtrs = state.TR.vtid_cnt; // 使用 vtid_cnt 作為 VT 索引
                int vprd = state.PS.vpsum_cnt; // 使用 vpsum_cnt 作為 VP 索引

                Vector vt = state.TR.getVT(vtrs);
                if(func3==2){ // VMUL family => 向量輸出
                    Vector oldv = state.PS.getVP64(vprd);
                    Vector newv = state.valu.vmul(vt, state.dmrv, oldv);
                    state.PS.setVP64(vprd, newv);

                    // 更新動態計數器 VP
                    if(vpstride=3) state.PS.vpsum_cnt = 0;
                    else state.PS.vpsum_cnt = (state.PS.vpsum_cnt + vpstride) & 0x1F;

                } else { // VMAC family => 累加到標量 P
                    Element oldp = state.PS.getP(prd);
                    Element newp = state.valu.vmac(vt, state.dmrv, oldp);
                    state.PS.setP(prd, newp);

                    // 更新動態計數器 P
                    if(pstride==31) state.PS.psum_cnt = 0;
                    else state.PS.psum_cnt = (state.PS.psum_cnt + pstride) & 0x1F;
                }

                // 更新動態計數器 VT
                if(vtstride==3) state.TR.vtid_cnt = 0;
                else state.TR.vtid_cnt = (state.TR.vtid_cnt + vtstride) % 3;

                if(func1){ S.DMA.next(); }
            } break;
            case 4: { // VPSUM : PLO = PLI + P[psum_cnt]
                int vprs = (w>>5)&0x1F;
                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());
            } break;
            case 5: { // VPSUMR : PLO = PLI + P[psum_cnt] 並移動 psum_cnt
                int vpstride = (w>>5)&0x1F;
                int vprs = state.PS.vpsum_cnt; // 使用 vpsum_cnt 作為 VP 索引

                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());

                // 更新動態計數器 VP
                if(vpstride==3) state.PS.vpsum_cnt = 0;
                else state.PS.vpsum_cnt = (state.PS.vpsum_cnt + vpstride) & 0x1F;
            } break;
            default: break;
        }
        return;
    }

    // SETRID
    if(opcode==2 && funct2==2){
        switch(func3){
            case 1: state.PS.psum_cnt = (w>>5)&0x1F; break;          // SETRID.P
            case 2: state.TR.vtid_cnt = (w>>10)&0x3; break;           // SETRID.T
            case 3: state.PS.psum_cnt = (w>>5)&0x1F; state.TR.vtid_cnt = (w>>10)&0x3; break; // SETRID.PT
            default: break;
        }
        return;
    }

    // CLEAR
    if(opcode==2 && funct2==3){
        switch(func3){
            case 0: state.TR.vtid_cnt = 0; break; // CLEAR.T
            case 1: state.PS.psum_cnt = 0; break; // CLEAR.P
            default: break;
        }
        return;
    }

    // JUMP (J)
    if(opcode==1 && funct2==2){
        int imm = ((func3 & 0x7)<<7) | (getFunc1(w)<<10) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
        state.pc = imm;
        return;
    }

    // LOOPIN / LOOPBREAK
    if(opcode==1 && funct2==3){
        if(getFunc1(w)==0){ // LOOPIN
            int lc = ((func3 &0x7)<<7) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
            state.loops.loopIn(state.pc+2, lc);
        } else { // LOOPBREAK
            state.loops.loopBreak();
        }
        return;
    }
}

void PESimulator::step(){

    if(state.halted) return;

    if(state.pc < 0 || state.pc >= state.IM.progSize()){
        std::cerr << "PC out of range: " << state.pc << " (max=" << state.IM.progSize() << ")" << std::endl;
        state.halted = true;
        return;
    }

    uint16_t inst = state.IM.fetch(state.pc);
    bool loopEnd = inst & 1;

    if(state.cfg.enable_trace){
        std::cout << "[CYC="<<state.cycles<<"] PC="<<state.pc
                  <<" INST=0x"<<std::hex<<std::setw(4)<<std::setfill('0')<<inst<<std::dec
                  <<"  "<<disasm(inst)<<"\n";
    }

    execute(inst);

    // 2. PC = PC + 2
    // 注意：這裡假設指令是 16-bit 對齊
    state.pc += 2;

    // 3. 處理 loop end flag
    if(loopEnd){
        uint16_t new_pc = state.pc;
        if(state.loops.handleLoopEndFlag(new_pc)){
            state.pc = new_pc;
        }
    }

    state.cycles++;
}

int run_simulator_cli(int argc, char **argv){
    std::string progFile; // 可為 .asm / .hex / .bin
    std::string dumpPath; // --dump <file|->
    bool trace=false; uint64_t max_cycles=0;
    for(int i=1;i<argc;i++){
        std::string a=argv[i];
        if(a=="-asm" && i+1<argc){ progFile=argv[++i]; }
        else if(a=="-trace"){ trace=true; }
        else if(a=="-max" && i+1<argc){ max_cycles=std::stoull(argv[++i]); }
        else if(a=="--dump" && i+1<argc){ dumpPath=argv[++i]; }
        else if(a=="-h"||a=="--help"){
            std::cout << "Usage: ha-sim -asm program.(asm|hex|bin) [-trace] [-max N] [--dump out.txt|-]\n";
            return 0;
        }
    }
    if(progFile.empty()){
        std::cerr << "No -asm file provided\n";
        return 1;
    }

    auto ends_with = [](const std::string &s, const std::string &suf){
        if(s.size()<suf.size()) return false; return std::equal(suf.rbegin(), suf.rend(), s.rbegin(), [](char a,char b){ return std::tolower(a)==std::tolower(b); }); };

    std::vector<uint16_t> prog;
    try {
        if(ends_with(progFile, ".hex")){
            prog = readHex(progFile);
        } else if(ends_with(progFile, ".bin")){
            prog = readBin(progFile);
        } else { // 視為 .asm
            std::string src = readAsm(progFile);
            Assembler assembler;
            prog = assembler.assemble(src, false);
        }
    } catch(const std::exception &e){
        std::cerr << "Assemble/Load error: "<<e.what()<<"\n";
        return 1;
    }

    PEConfig cfg; cfg.enable_trace=trace;
    if(prog.size() > (size_t)cfg.im_words){
        std::cerr << "Program size "<<prog.size()<<" words exceeds IM capacity "<<cfg.im_words<<" words\n";
        return 1;
    }

    // 新增: 印出 IM 使用率 (以位元組)
    {
        size_t used_bytes = prog.size() * sizeof(uint16_t); // 16-bit word = 2 bytes
        size_t cap_bytes  = (size_t)cfg.im_words * sizeof(uint16_t);
        double usage_pct = (cap_bytes==0)?0.0: (double)used_bytes * 100.0 / (double)cap_bytes;
        std::ios old_state(nullptr); old_state.copyfmt(std::cout);
        std::cout << "IM usage: " << used_bytes << "/" << cap_bytes
                  << " bytes (" << std::fixed << std::setprecision(2) << usage_pct << "%)\n";
        std::cout.copyfmt(old_state);
    }

    // 若需要輸出 disasm list
    if(!dumpPath.empty()){
        Disassembler dis;
        std::ostream *osp = &std::cout; std::ofstream ofs;
        if(dumpPath != "-"){
            ofs.open(dumpPath);
            if(!ofs){ std::cerr << "Cannot open dump output: "<<dumpPath<<"\n"; return 1; }
            osp = &ofs;
        }
        for(size_t i=0;i<prog.size();++i){
            *osp << std::setw(4) << i << ": " << dis.disasmWord(prog[i]) << "\n";
        }
    }

    // 新增: 設定 PortIO
    PortIO port_io;

    PESimulator sim(cfg);
    sim.loadProgram(prog);
    sim.connectPortIO(&port_io); // 以指標連接

    std::cout << "Starting simulation...\n";
    sim.run(max_cycles);

    auto &S = sim.getState();
    std::cout << "\n\nSimulation Halted\n";
    std::cout << "------------------\n";
    std::cout << "Total Cycles: " << S.cycles << "\n";
    std::cout << "Final PC: 0x" << std::hex << S.pc << std::dec << "\n";
    std::cout << "Loop Stack is empty: " << (S.loops.empty() ? "Yes" : "No") << "\n";
    std::cout << "Halted State: " << (S.halted ? "Yes" : "No") << "\n";
    std::cout << "------------------\n";
    return 0;
}

} // namespace hybridacc
