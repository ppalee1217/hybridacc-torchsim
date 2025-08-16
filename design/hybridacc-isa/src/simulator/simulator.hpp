#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <queue> // 新增 queue
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
        void reset(){ PD = {}; PS = {}; PIL = {}; POL = {}; }
        void pushPD(uint16_t v){ PD.push(v); }
        void pushPS(uint64_t v){ PS.push(v); }
        void pushPIL(uint64_t v){ PIL.push(v); }
        void pushPOL(uint64_t v){ POL.push(v); }

        bool popPD(uint16_t &v);
        bool popPS(uint64_t &v);
        bool popPIL(uint64_t &v);
        std::vector<uint16_t> getPD() const;
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
