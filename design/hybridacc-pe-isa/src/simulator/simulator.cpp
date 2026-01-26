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
#include <filesystem> // 新增: 使用 PortIO::loadDir / savePOL 所需

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
        return false;
        // return true;
    }
    v = PD.front(); PD.pop();
    return true;
}
bool PortIO::popPS(uint64_t &v) {
    if(PS.empty()) {
        std::cerr << "PS queue is empty" << std::endl;
        v = 0;
        return false;
        // return true;
    }
    v = PS.front(); PS.pop();
    return true;
}
bool PortIO::popPIL(uint64_t &v) {
    if(PIL.empty()) {
        std::cerr << "PIL queue is empty" << std::endl;
        v = 0;
        return false;
        // return true;
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

// 新增: 從資料夾載入測試資料
bool PortIO::loadDir(const std::string &dir){
    namespace fs = std::filesystem;
    // 先清空既有內容
    PD = {}; PS = {}; PIL = {}; POL = {};

    if(!fs::exists(dir) || !fs::is_directory(dir)){
        std::cerr << "[PortIO] data directory not found: " << dir << "\n";
        return false; // 目錄本身不存在才回傳 false
    }

    auto load_words16 = [&](const fs::path &p){
        std::ifstream ifs(p, std::ios::binary);
        if(!ifs){ std::cerr << "[PortIO] cannot open file: " << p << "\n"; return; }
        uint16_t v; while(ifs.read(reinterpret_cast<char*>(&v), sizeof(v))){ PD.push(v); }
        ifs.close();
    };
    auto load_words64_queue = [&](const fs::path &p, std::queue<uint64_t> &q){
        std::ifstream ifs(p, std::ios::binary);
        if(!ifs){ std::cerr << "[PortIO] cannot open file: " << p << "\n"; return; }
        uint64_t v; while(ifs.read(reinterpret_cast<char*>(&v), sizeof(v))){ q.push(v); }
        ifs.close();
    };
    auto load_words64_vec = [&](const fs::path &p, std::vector<uint64_t> &vec){
        std::ifstream ifs(p, std::ios::binary);
        if(!ifs){ std::cerr << "[PortIO] cannot open file: " << p << "\n"; return; }
        uint64_t v; while(ifs.read(reinterpret_cast<char*>(&v), sizeof(v))){ vec.push_back(v); }
        ifs.close();
    };

    fs::path base(dir);
    fs::path f_act_in = base/"activation_input.bin";   // PD
    fs::path f_weight = base/"weight.bin";              // PS
    fs::path f_ps_in  = base/"ps_input.bin";            // PIL

    bool missing=false;
    if(fs::exists(f_act_in)) load_words16(f_act_in); else { std::cerr<<"[PortIO] warning: "<<f_act_in.filename()<<" missing\n"; missing=true; }
    if(fs::exists(f_weight)) load_words64_queue(f_weight, PS); else { std::cerr<<"[PortIO] warning: "<<f_weight.filename()<<" missing\n"; missing=true; }
    if(fs::exists(f_ps_in))  load_words64_queue(f_ps_in, PIL); else { std::cerr<<"[PortIO] warning: "<<f_ps_in.filename()<<" missing\n"; missing=true; }

    std::cout << "[PortIO] loaded data from "<<dir << "\n";
    std::cout  << "[PortIO] PD="<<PD.size() << " PS="<<PS.size()
              << " PIL="<<PIL.size() << (missing?" (partial)":"") << "\n";
    return true; // 只要目錄存在即視為成功
}

// 新增: 將 POL 內容輸出為 .bin
bool PortIO::savePOL(const std::string &filepath) const {
    std::ofstream ofs(filepath, std::ios::binary);
    if(!ofs){
        std::cerr << "[PortIO] cannot open output file: " << filepath << "\n";
        return false;
    }
    auto tmp = POL; // 複製一份，避免改變原佇列
    while(!tmp.empty()){
        uint64_t v = tmp.front(); tmp.pop();
        ofs.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    ofs.close();
    std::cout << "[PortIO] saved POL to " << filepath << " (words=" << getPOL().size() << ")\n";
    return true;
}

// ----------------- 新增: PESimulator 基本介面實作 -----------------
PESimulator::PESimulator(const PEConfig &cfg){
    state.cfg = cfg;
    state.IM.resize(cfg.im_size);
    state.DM.resize(cfg.dm_size);
    state.LDMA.init(&state.DM, &state.dmrv, &state.dmwv, cfg.dma_latency);
    state.SDMA.init(&state.DM, &state.dmrv, &state.dmwv, cfg.dma_latency);
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
PESimulator::ExecStatus PESimulator::execute(uint16_t w) {
    int opcode = getOpcode(w);
    int funct2 = getFunct2(w);
    int func3  = getFunc3(w);
    int func1  = getFunc1(w);
    int payload= getPayload(w);

    auto &S = state;

    // HALT
    if(opcode==3 && funct2==3 && func3==0){ S.halted = true; return ExecStatus::NEXT; }
    // SWAPDM
    if(opcode==3 && funct2==3 && func3==4){ // opcode 11 funct2 11 func3 100
        // Wait for SDMA busy or Back buffer not valid (not yet filled)
        if(S.SDMA.busy() || !S.DM.isValid(S.DM.getBackBank())){
            return ExecStatus::STALL; // Stall, do not advance PC
        }
        S.DM.swap();
        return ExecStatus::NEXT;
        // Advance PC (fall through to end of function)
    } else if(opcode==3 && funct2==3) {
        // Other system instructions handled or reserved
    }

    // NOP
    if(opcode==2 && funct2==0){ return ExecStatus::NEXT; }

    // DMA Setup (Opcode 00)
    // f2=00: SDMA, f2=01: LDMA
    if(opcode==0 && (funct2==0 || funct2==1)){
        int bits6_1 = payload & 0x3F;
        int bit0 = (payload >> 6) & 0x1;
        int val = (func3<<7) | (bits6_1<<1) | bit0; // 10-bit

        if(funct2==1) { // LDMA
            if(func1==0) S.LDMA.updateBase(val); else S.LDMA.updateLen(val);
        } else { // SDMA
            if(func1==0) S.SDMA.updateBase(val); else S.SDMA.updateLen(val);
        }
        return ExecStatus::NEXT;
    }

    // LDMA Operations (Opcode 00, f2=10)
    if(opcode==0 && funct2==2){
        int stride = (w>>10)&0x7;
        switch(func3){
            case 0: S.LDMA.issue(DMARequestType::LOAD_BYTE, stride, false); break; // LB
            case 1: S.LDMA.issue(DMARequestType::LOAD_HALF, stride, false); break; // LH
            case 2: S.LDMA.issue(DMARequestType::LOAD_WORD, stride, false); break; // LW
            case 3: S.LDMA.issue(DMARequestType::LOAD_DWORD, stride, false); break; // LD
            case 4: S.LDMA.issue(DMARequestType::LOAD_BYTE, stride, true ); break; // LBB
            case 5: S.LDMA.issue(DMARequestType::LOAD_HALF, stride, true ); break; // LHB
            case 6: S.LDMA.issue(DMARequestType::LOAD_WORD, stride, true ); break; // LWB
            default: break;
        }
        return ExecStatus::NEXT;
    }

    // SDMA Operations (Opcode 00, f2=11)
    if(opcode==0 && funct2==3){
        if(func3==3){
            int stride = (w>>10)&0x7;
            uint64_t ps_word = 0;
            if (!port_io || !port_io->popPS(ps_word)) {
                throw std::runtime_error("Blocking read failed: PS port is empty");
            }
            S.dmwv.fromUint64(ps_word); // Get data from PS port

            S.SDMA.issue(DMARequestType::STORE_DWORD, stride, false); // SD

            // Note: In blocking design, we might spin here. But to support concurrent execution
            // SDMA should ideally run in background. For now, we keep blocking behavior for data consumption
            // but tracking transfer size needs care.
            while(S.SDMA.busy()) {
                 if (!port_io->popPS(ps_word)) {
                     throw std::runtime_error("Blocking read failed: PS port is empty");
                 }
                 S.dmwv.fromUint64(ps_word);
                 S.SDMA.next();
            }
        }
        return ExecStatus::NEXT;
    }

    // Config LOOP (Opcode 01, f2=01)
    if(opcode==1 && funct2==1){
        int bits6_1 = payload & 0x3F;
        int bit0 = (payload >> 6) & 0x1;
        int val = (func3<<7) | (bits6_1<<1) | bit0; // 10-bit
        val += 1; // 0-based
        if(func1==0) S.LDMA.setLoop(val); // LDMA.LOOP
        else S.SDMA.setLoop(val);         // SDMA.LOOP
        return ExecStatus::NEXT;
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
                case 0: // K3 -> 011011011011b (11 ~ 0)
                    maskBits = 0b011011011011; break;
                case 1: // K5 -> 001111001111b (11 ~ 0)
                    maskBits = 0b001111001111; break;
                case 2: // K7 -> 000000111111b (11 ~ 0)
                    maskBits = 0b000000111111; break;
                default: maskBits = 0; break; // 未定義 code 清空
            }
            state.TR.shift(maskBits); // 將 T registers 向右移動
        }
        return ExecStatus::NEXT;
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
                if(func1){ S.LDMA.next(); } // N 變形: 允許下一個 DMA (Usually LDMA)
            } break;
            case 1: // VMACR / VMACRN : P[psum_cnt] += dot(VT[vtid_cnt], DMRV)
            case 3: { // VMULR / VMULRN : VP[vpsum_cnt] += mul(VT[vtid_cnt], DMRV)
                int pstride = (w>>5)&0x1F;
                int vpstride = (w>>5)&0x1F;
                int vtstride = (w>>10)&0x3;
                // 取得向量 VT
                int prd = state.PS.psum_cnt; // 使用 psum_cnt 作為 P 索引
                int vtrs = state.TR.vtid_cnt; // 使用 vtid_cnt 作為 VT 索引
                // int vprd = state.PS.vpsum_cnt; // 使用 vpsum_cnt 作為 VP 索引

                Vector vt = state.TR.getVT(vtrs);
                if(func3==3){ // VMUL family => 向量輸出
                    Vector oldv = state.PS.getVP64(prd);
                    Vector newv = state.valu.vmul(vt, state.dmrv, oldv);
                    state.PS.setVP64(prd, newv);
                    // 更新動態計數器 VP
                    if(vpstride==31) state.PS.psum_cnt = 0;
                    else state.PS.psum_cnt = (state.PS.psum_cnt + vpstride) & 0x1F;

                } else { // VMAC family => 累加到標量 P
                    Element oldp = state.PS.getP(prd);
                    Element newp = state.valu.vmac(vt, state.dmrv, oldp);
                    state.PS.setP(prd, newp);

                    // std::cerr << "[PESimulator] VMACR/VMACRN: P[" << prd << "] = 0x" << std::hex <<  newp << std::dec;
                    // std::cerr << ", VT[" << vtrs << "] = " << std::hex << vt.toUint64() << ", DMRV = " << state.dmrv.toUint64() << std::dec << "\n";

                    // 更新動態計數器 P
                    if(pstride==31) state.PS.psum_cnt = 0;
                    else state.PS.psum_cnt = (state.PS.psum_cnt + pstride) & 0x1F;
                }

                // 更新動態計數器 VT
                if(vtstride==3) state.TR.vtid_cnt = 0;
                else state.TR.vtid_cnt = (state.TR.vtid_cnt + vtstride) % 3;

                if(func1){ S.LDMA.next(); }
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
                int vprs = state.PS.psum_cnt; // 使用 vpsum_cnt 作為 VP 索引

                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());

                // 更新動態計數器 VP
                if(vpstride==31) state.PS.psum_cnt = 0;
                else state.PS.psum_cnt = (state.PS.psum_cnt + vpstride) & 0x1F;
            } break;
            default: break;
        }
        return ExecStatus::NEXT;
    }

    // SETRID
    if(opcode==2 && funct2==2){
        switch(func3){
            case 1: state.PS.psum_cnt = (w>>5)&0x1F; break;          // SETRID.P
            case 2: state.TR.vtid_cnt = (w>>10)&0x3; break;           // SETRID.T
            case 3: state.PS.psum_cnt = (w>>5)&0x1F; state.TR.vtid_cnt = (w>>10)&0x3; break; // SETRID.PT
            default: break;
        }
        return ExecStatus::NEXT;
    }

    // CLEAR
    if(opcode==2 && funct2==3){
        switch(func3){
            case 0: state.TR.clear(); break; // CLEAR.T
            case 1: state.PS.clear(); break; // CLEAR.P
            default: break;
        }
        return ExecStatus::NEXT;
    }

    // JUMP (J)
    if(opcode==1 && funct2==2){
        int imm = ((func3 & 0x7)<<7) | (getFunc1(w)<<10) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
        state.pc = imm;
        return ExecStatus::JUMP;
    }

    // LOOPIN / LOOPBREAK
    if(opcode==1 && funct2==3){
        if(getFunc1(w)==0){ // LOOPIN
            int lc = ((func3 &0x7)<<7) | (((w>>11)&1)) | ((payload & 0x3F)<<1);
            lc += 1; // 0-based
            state.loops.loopIn(state.pc+2, lc);
        } else { // LOOPBREAK
            state.loops.loopBreak();
        }
        return ExecStatus::NEXT;
    }
    return ExecStatus::NEXT;
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

    ExecStatus status = execute(inst);

    if(status == ExecStatus::NEXT){
        // 2. PC = PC + 2
        // 注意：這裡假設指令是 16-bit 對齊
        state.pc += 2;
    }

    if(status != ExecStatus::STALL){
         // 3. 處理 loop end flag
        if(loopEnd){
            uint16_t new_pc = state.pc;
            if(state.loops.handleLoopEndFlag(new_pc)){
                state.pc = new_pc;
            }
        }
    }

    state.cycles++;
}

int run_simulator_cli(int argc, char **argv){
    std::string progFile; // 可為 .asm / .hex / .bin
    std::string dumpPath; // --dump <file|->
    bool trace=false; uint64_t max_cycles=0;
    std::string dataDir; // 新增: 資料夾
    std::string jsonReportPath; // (預留, 目前未使用)
    std::string polOutPath; // 新增: 輸出 POL 的檔案路徑
    for(int i=1;i<argc;i++){
        std::string a=argv[i];
        if(a=="-asm" && i+1<argc){ progFile=argv[++i]; }
        else if(a=="-trace"){ trace=true; }
        else if(a=="-max" && i+1<argc){ max_cycles=std::stoull(argv[++i]); }
        else if(a=="--dump" && i+1<argc){ dumpPath=argv[++i]; }
        else if(a=="--data" && i+1<argc){ dataDir=argv[++i]; }
        else if(a=="--pol" && i+1<argc){ polOutPath = argv[++i]; }
        else if(a=="-h"||a=="--help"){
            std::cout << "Usage: ha-sim -asm program.(asm|hex|bin) [-trace] [-max N] [--dump out.txt|-] [--data datadir] [--pol pol_output.bin]" << "\n";
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

    PEConfig cfg;
    cfg.enable_trace=trace;
    if(prog.size() * sizeof(uint16_t) > (size_t)cfg.im_size ){
        std::cerr << "Program size "<<prog.size() << "(Bytes) exceeds IM capacity "<<cfg.im_size <<" (Bytes)\n";
        return 1;
    }

    // 新增: 印出 IM 使用率 (以位元組)
    {
        size_t used_bytes = prog.size() * sizeof(uint16_t); // 16-bit word = 2 bytes
        size_t cap_bytes  = (size_t)cfg.im_size;
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
    if(!dataDir.empty()){
        if(!port_io.loadDir(dataDir)){
            std::cerr << "Failed to load data directory: "<<dataDir<<"\n";
            return 1;
        }
    }

    PESimulator sim(cfg);
    sim.loadProgram(prog);
    sim.connectPortIO(&port_io); // 以指標連接

    std::cout << "Starting simulation...\n";
    sim.run(max_cycles);

    // 新增: 若指定輸出 POL 則寫檔
    if(!polOutPath.empty()){
        if(!port_io.savePOL(polOutPath)){
            std::cerr << "Failed to save POL to: " << polOutPath << "\n";
        }
    }

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
