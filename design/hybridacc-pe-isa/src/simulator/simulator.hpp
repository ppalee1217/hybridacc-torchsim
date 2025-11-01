#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <queue> // 新增 queue
#include <filesystem> // 新增: for loadDir
#include "component.hpp"
#include "../assambler/instruction.hpp"
#include "../assambler/utils.hpp"

namespace hybridacc {

// fifo port for PE
class PortIO{
    public:
        PortIO() = default;
        PortIO(const std::vector<uint16_t>& pd, const std::vector<uint64_t>& ps,
               const std::vector<uint64_t>& pil, const std::vector<uint64_t>& pol){
            for(auto v: pd) PD.push(v);
            for(auto v: ps) PS.push(v);
            for(auto v: pil) PIL.push(v);
            for(auto v: pol) POL.push(v);
        }
        void reset(){ PD = {}; PS = {}; PIL = {}; POL = {};}
        void pushPD(uint16_t v){ PD.push(v); }
        void pushPS(uint64_t v){ PS.push(v); }
        void pushPIL(uint64_t v){ PIL.push(v); }
        void pushPOL(uint64_t v){ POL.push(v); }
        bool popPD(uint16_t &v);
        bool popPS(uint64_t &v);
        bool popPIL(uint64_t &v);
        std::vector<uint16_t> getPD() const;

        // 新增: 從資料夾載入測試資料
        // 檔案對應:
        //  activation_input.bin  -> PD (16-bit elements)
        //  weight.bin            -> PS (64-bit words)
        //  ps_input.bin          -> PIL (64-bit words)
        // 若部分檔案缺失不視為錯誤 (回傳 true 但列警告)
        bool loadDir(const std::string &dir);
        std::vector<uint64_t> getPOL() const { // 取得實際輸出
            std::vector<uint64_t> out; auto tmp = POL; while(!tmp.empty()){ out.push_back(tmp.front()); tmp.pop(); } return out; }
        // 新增: 將 POL 內容以 64-bit little-endian 寫出成二進位檔 (.bin)
        bool savePOL(const std::string &filepath) const; // 回傳是否成功
    private:
        std::queue<uint16_t> PD;
        std::queue<uint64_t> PS;
        std::queue<uint64_t> PIL;
        std::queue<uint64_t> POL;
}; // <-- 補上分號

class PESimulator {
public:
    explicit PESimulator(const PEConfig &cfg);
    void loadProgram(const std::vector<uint16_t>& prog);
    void connectPortIO(PortIO* p){ port_io = p; }
    PEState& getState();
    void run(uint64_t max_cycles=0);
    void step();
private:
    PortIO* port_io {nullptr};
    PEState state;
    void execute(uint16_t inst);
    std::string disasm(uint16_t w){ return Disassembler().disasmWord(w); }

};


int run_simulator_cli(int argc, char **argv);

} // namespace hybridacc
