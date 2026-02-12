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
static inline int getFunc1(uint16_t w){ return (w>>5) & 0x1; }
static inline int getPayload(uint16_t w){ return (w>>6) & 0x3FF; }


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
    state.opcode_exec_count = {0,0,0,0};
}

void PESimulator::loadProgram(const std::vector<uint16_t>& prog){
    state.IM.load(prog);
    state.pc = 0;
    state.halted = false;
    state.cycles = 0;
    state.opcode_exec_count = {0,0,0,0};
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
    int func1  = getFunc1(w);
    int payload= getPayload(w);
    int func3  = payload & 0x7;
    int reg5   = (payload >> 5) & 0x1F;
    int vtbits = (payload >> 3) & 0x3;
    int stride = (payload >> 3) & 0x7;

    auto &S = state;

    // HALT (opcode=10 func2=11)
    if(opcode==2 && funct2==3 && func1==0){ S.halted = true; return ExecStatus::NEXT; }

    // SYS.SYNC (opcode=10 func2=01 func1=1)
    if(opcode==2 && funct2==1 && func1==1){
        if(payload & 0x1){
            if(S.SDMA.busy() || !S.DM.isValid(S.DM.getBackBank())){
                return ExecStatus::STALL; // Stall, do not advance PC
            }
            S.DM.swap();
        }
        return ExecStatus::NEXT;
    }

    // SYS.CTRL (opcode=10 func2=01 func1=0)
    if(opcode==2 && funct2==1 && func1==0){
        bool sdma_act = payload & (1<<7);
        bool sdma_rst = payload & (1<<6);
        bool ldma_act = payload & (1<<5);
        bool ldma_rst = payload & (1<<4);
        bool rst_pid  = payload & (1<<3);
        bool rst_tid  = payload & (1<<2);
        bool clr_t    = payload & (1<<1);
        bool clr_p    = payload & (1<<0);

        if(clr_t) S.TR.clear();
        if(clr_p) S.PS.clear();
        if(rst_pid){ S.PS.psum_cnt = 0; S.PS.vpsum_cnt = 0; }
        if(rst_tid){ S.TR.tid_cnt = 0; S.TR.vtid_cnt = 0; }

        if(ldma_rst) S.LDMA.resetActive();
        if(sdma_rst) S.SDMA.resetActive();

        if(ldma_act) S.LDMA.activateFromStatic();
        if(sdma_act){
            uint64_t ps_word = 0;
            if (!port_io || !port_io->popPS(ps_word)) {
                throw std::runtime_error("Blocking read failed: PS port is empty");
            }
            S.dmwv.fromUint64(ps_word);
            S.SDMA.activateFromStatic();
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

    // NOP (opcode=10 func2=10)
    if(opcode==2 && funct2==2 && func1==0){ return ExecStatus::NEXT; }

    // DMA Setup (Opcode 00)
    if(opcode==0 && funct2==0){
        if(func1==0) S.LDMA.updateBase(payload);
        else S.SDMA.updateBase(payload);
        return ExecStatus::NEXT;
    }
    if(opcode==0 && funct2==1){
        if(func1==0) S.LDMA.updateLen(payload+1);
        else S.SDMA.updateLen(payload+1);
        return ExecStatus::NEXT;
    }
    if(opcode==0 && funct2==2){
        if(func1==0) S.LDMA.setLoop(payload+1);
        else S.SDMA.setLoop(payload+1);
        return ExecStatus::NEXT;
    }

    // DMA Operations / TSTORE / VTSTORE / TSHIFT (Opcode 00, func2=11)
    if(opcode==0 && funct2==3){
        if(func1==0){
            switch(func3){
                case 0: S.LDMA.issue(DMARequestType::LOAD_BYTE, stride, false); break; // LB
                case 1: S.LDMA.issue(DMARequestType::LOAD_HALF, stride, false); break; // LH
                case 2: S.LDMA.issue(DMARequestType::LOAD_WORD, stride, false); break; // LW
                case 3: S.LDMA.issue(DMARequestType::LOAD_DWORD, stride, false); break; // LD
                case 4: S.LDMA.issue(DMARequestType::LOAD_BYTE, stride, true ); break; // LBB
                case 5: S.LDMA.issue(DMARequestType::LOAD_HALF, stride, true ); break; // LHB
                case 6: S.LDMA.issue(DMARequestType::LOAD_WORD, stride, true ); break; // LWB
                case 7: S.SDMA.issue(DMARequestType::STORE_DWORD, stride, false); break; // SD
                default: break;
            }
            return ExecStatus::NEXT;
        } else {
            if(func3==0){ // TSTORE
                int trd = (payload>>6)&0xF;
                uint16_t val = 0;
                port_io->popPD(val);
                state.TR.setT(trd, val);
            } else if(func3==1){ // VTSTORE
                int vtrd = (payload>>3)&0x3;
                for(int i=0;i<4;++i){
                    uint16_t val = 0;
                    port_io->popPD(val);
                    state.TR.setT(vtrd + i*3, val);
                }
            } else if(func3==2){ // TSHIFT
                int k = (payload>>3)&0x7;
                uint16_t maskBits = 0;
                switch(k){
                    case 0: maskBits = 0b011011011011; break; // k3
                    case 1: maskBits = 0b001111001111; break; // k5
                    case 2: maskBits = 0b000000111111; break; // k7
                    default: maskBits = 0; break;
                }
                state.TR.shift(maskBits);
            }
            return ExecStatus::NEXT;
        }
    }

    // Arithmetic Group (opcode=01)
    if(opcode==1){
        if(funct2==0){
            if(func3==0){ // VMAC / VMACN
                int prd = reg5;
                int vtrs = vtbits;
                Vector vt = state.TR.getVT(vtrs);
                Element oldp = state.PS.getP(prd);
                Element newp = state.valu.vmac(vt, state.dmrv, oldp);
                state.PS.setP(prd, newp);
                if(func1){ S.LDMA.next(); }
            } else if(func3==1){ // VMACR / VMACRN
                int pstride = reg5;
                int vtstride = vtbits;
                int prd = state.PS.psum_cnt;
                int vtrs = state.TR.vtid_cnt;
                Vector vt = state.TR.getVT(vtrs);
                Element oldp = state.PS.getP(prd);
                Element newp = state.valu.vmac(vt, state.dmrv, oldp);
                state.PS.setP(prd, newp);
                if(pstride==31) state.PS.psum_cnt = 0;
                else state.PS.psum_cnt = (state.PS.psum_cnt + pstride) & 0x1F;
                if(vtstride==3) state.TR.vtid_cnt = 0;
                else state.TR.vtid_cnt = (state.TR.vtid_cnt + vtstride) % 3;
                if(func1){ S.LDMA.next(); }
            }
        } else if(funct2==1){
            if(func3==0){ // VMUL / VMULN
                int prd = reg5;
                int vtrs = vtbits;
                Vector vt = state.TR.getVT(vtrs);
                Vector oldv = state.PS.getVP64(prd);
                Vector newv = state.valu.vmul(vt, state.dmrv, oldv);
                state.PS.setVP64(prd, newv);
                if(func1){ S.LDMA.next(); }
            } else if(func3==1){ // VMULR / VMULRN
                int vpstride = reg5;
                int vtstride = vtbits;
                int prd = state.PS.psum_cnt;
                int vtrs = state.TR.vtid_cnt;
                Vector vt = state.TR.getVT(vtrs);
                Vector oldv = state.PS.getVP64(prd);
                Vector newv = state.valu.vmul(vt, state.dmrv, oldv);
                state.PS.setVP64(prd, newv);
                if(vpstride==31) state.PS.psum_cnt = 0;
                else state.PS.psum_cnt = (state.PS.psum_cnt + vpstride) & 0x1F;
                if(vtstride==3) state.TR.vtid_cnt = 0;
                else state.TR.vtid_cnt = (state.TR.vtid_cnt + vtstride) % 3;
                if(func1){ S.LDMA.next(); }
            }
        } else if(funct2==2){
            if(func3==0){ // VPSUM
                int vprs = reg5;
                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());
            } else if(func3==1){ // VPSUMR
                int vpstride = reg5;
                int vprs = state.PS.psum_cnt;
                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());
                if(vpstride==31) state.PS.psum_cnt = 0;
                else state.PS.psum_cnt = (state.PS.psum_cnt + vpstride) & 0x1F;
            }
        } else if(funct2==3){
            if(func3==0){ // VPSUM_VTSTORE
                // VPSUM
                int vprs = reg5;
                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());
                // VTSTORE
                int vtrd = (payload>>3)&0x3;
                for(int i=0;i<4;++i){
                    uint16_t val = 0;
                    port_io->popPD(val);
                    state.TR.setT(vtrd + i*3, val);
                }
            } else if(func3==1){ // VPSUMR_VTSTORE
                // VPSUMR
                int vpstride = reg5;
                int vprs = state.PS.psum_cnt;
                Vector vt;
                Vector psum = state.PS.getVP64(vprs);
                uint64_t pil_word=0;
                if(!port_io || !port_io->popPIL(pil_word)) throw std::runtime_error("PIL empty");
                vt.fromUint64(pil_word);
                Vector pout = state.valu.vadd(psum, vt);
                if(port_io) port_io->pushPOL(pout.toUint64());
                if(vpstride==31) state.PS.psum_cnt = 0;
                else state.PS.psum_cnt = (state.PS.psum_cnt + vpstride) & 0x1F;
                // VTSTORE
                int vtrd = (payload>>3)&0x3;
                for(int i=0;i<4;++i){
                    uint16_t val = 0;
                    port_io->popPD(val);
                    state.TR.setT(vtrd + i*3, val);
                }
            }
        }
        return ExecStatus::NEXT;
    }

    // LOOPIN / LOOPBREAK (opcode=10 func2=00)
    if(opcode==2 && funct2==0){
        if(func1==0){
            state.loops.loopIn(state.pc+2, payload+1);
        } else {
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
        int opcode = getOpcode(inst);
        if(opcode >= 0 && opcode < 4){
            state.opcode_exec_count[opcode] += 1;
        }
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
    {
        uint64_t total_exec = 0;
        for(auto c : S.opcode_exec_count) total_exec += c;
        const char* labels[4] = {"Data Movement", "Arithmetic", "Control Flow", "System-Level"};
        std::cout << "Opcode Execute Count/Ratio:\n";
        for(int op=0; op<4; ++op){
            double ratio = (total_exec==0) ? 0.0 : (100.0 * (double)S.opcode_exec_count[op] / (double)total_exec);
            std::cout << "  opcode " << op << " (" << labels[op] << "): " << S.opcode_exec_count[op]
                      << " (" << std::fixed << std::setprecision(2) << ratio << "%)\n";
        }
        std::cout.unsetf(std::ios::floatfield);
    }
    std::cout << "------------------\n";
    return 0;
}

} // namespace hybridacc
