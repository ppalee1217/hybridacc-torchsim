#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <deque>
#include <string>
#include <optional>
#include <cmath>
#include <limits>

namespace hybridacc {

struct PEConfig {
    int im_words = 256;      // Instruction memory capacity (16-bit words)
    int dm_words = 256;      // Data memory capacity (16-bit words)
    int dma_latency = 1;     // Cycles from issuing DMA load until data visible in DMRV
    bool enable_trace = false;
};

typedef uint16_t Element; // 元素類型 (16-bit half precision)

class Vector {
public:
    Vector() { lanes.fill(0); }
    Vector(const std::array<Element, 4>& l) : lanes(l) {}
    Element& operator[](size_t idx) { return lanes[idx]; }
    const Element& operator[](size_t idx) const { return lanes[idx]; }
    uint64_t toUint64() const {
        uint64_t result = 0;
        for (size_t i = 0; i < lanes.size(); ++i) {
            result |= static_cast<uint64_t>(lanes[i]) << (i * 16);
        }
        return result;
    }
    void fromUint64(uint64_t value) {
        for (size_t i = 0; i < lanes.size(); ++i) {
            lanes[i] = static_cast<Element>((value >> (i * 16)) & 0xFFFF);
        }
    }
    std::array<Element, 4> lanes; // 4-lane vector
};


// VALU component，operation:VMAC/VMUL/VPSUM
class VALU {
    public:
        VALU(): latency(1) {}
        void setLatency(int lat) { latency = lat; }
        int getLatency() const { return latency; }
        Element vmac(const Vector& a, const Vector& b, const Element& c);
        Vector vmul(const Vector& a, const Vector& b, const Vector& c);
        Vector vadd(const Vector& a, const Vector& b);
    private:
        uint16_t fp16_mul(uint16_t a, uint16_t b);
        uint16_t fp16_add(uint16_t a, uint16_t b);
        int latency; // cycles for operation
};

// Instruction Memory
class InstructionMemory {
public:
    explicit InstructionMemory(int words = 256): mem(words, 0), program_size(0) {}
    void resize(int words){ mem.assign(words, 0); program_size = 0; }
    void load(const std::vector<uint16_t>& prog);
    uint16_t fetch(int pc) const { return mem[pc / sizeof(uint16_t)]; }
    int size() const { return (int)mem.size() * sizeof(uint16_t); }
    int progSize() const { return (int)program_size; }
private:
    std::vector<uint16_t> mem;
    size_t program_size;
};

// Data Memory (element size 16-bit, bandwidth 64-bit)
class DataMemory {
public:
    explicit DataMemory(int words = 256): mem(words*sizeof(uint16_t), 0) {} // 16bits
    void resize(int words) { mem.resize(words*sizeof(uint16_t), 0); }
    uint64_t readWord(int idx) const; // 加上 const
    void writeWord(int idx, uint64_t v, uint8_t mask);
    int size() const { return (int)(mem.size()); }
    const std::vector<uint8_t>& raw() const { return mem; }
private:
    std::vector<uint8_t> mem;
};

// Transform Register File (unified representation for scalar T and vector selector VT)
class TransformRegFile {
public:
    int tid_cnt;
    int vtid_cnt;
    void reset() {reg.fill(0); tid_cnt = 0; vtid_cnt = 0;}
    void setT(int tid, Element v) { reg[tid] = v; }
    Element getT(int tid) const { return reg[tid]; }
    Vector getVT(int vtid) const {
        Vector vt;
        for (int i = 0; i < 4; ++i) {
            vt.lanes[i] = reg[vtid + i * 3];
        }
        return vt;
    }
    void shift(int maskBits);
private:
    std::array<Element, 12> reg{}; // 4x3 = 12 registers (3 for each vector lane)
};

// Psum Register File
class PsumRegFile {
public:
    int psum_cnt; // 32 partial sum registers
    int vpsum_cnt; // 8 vector partial sum registers (64-bit)
    std::array<Element,32> P{};    // Partial sum scalar
    void reset() {
        P.fill(0);
        for(auto &v: VP64){ v = Vector(); }
        psum_cnt = 0; vpsum_cnt = 0;
    }
    void setP(int pid, Element v) { P[pid] = v; }
    Element getP(int pid) const { return P[pid]; }
    void setVP64(int vpid, const Vector& v) {
        if (vpid < 8){
            for (int i = 0; i < 4; ++i) {
                P[vpid * 3 + i] = v.lanes[i]; // 4 scalar registers for vector lanes
            }
        } else {
            VP64[vpid - 8] = v; // 24 vector registers
        }
    }
    Vector getVP64(int vpid) const {
        if (vpid < 8) {
            Vector v;
            for (int i = 0; i < 4; ++i) {
                v.lanes[i] = P[vpid * 3 + i];
            }
            return v;
        } else {
            return VP64[vpid - 8];
        }
    }
private:
    std::array<Vector, 24> VP64{}; // Vector partial sum (64-bit)
};

struct LoopFrame { uint16_t start_pc; uint16_t remaining; };

class LoopController {
public:
    void loopIn(uint16_t start_pc, uint16_t count);
    void loopBreak();
    bool empty() const { return stack.empty(); }
    bool handleLoopEndFlag(uint16_t &pc_after_increment);
private:
    std::vector<LoopFrame> stack;
};

enum class DMARequestType {
    LOAD_BYTE, // 8-bit load
    LOAD_HALF, // 16-bit load
    LOAD_WORD, // 32-bit load
    LOAD_DWORD, // 64-bit load
    STORE_DWORD, // 64-bit store
};

class DMAController {
public:
    DMAController(): dm(nullptr), dmrv(nullptr), dmwv(nullptr), latency(1) {}
    void init(DataMemory* dm_, Vector* dmrv_, Vector* dmwv_,int lat){ dm = dm_; dmrv = dmrv_; dmwv = dmwv_; latency = lat; }

    void setBase(uint16_t base){ dma_base = base; dma_offset = 0; }
    void setLen(uint16_t len){ dma_len = len; }
    void setStride(uint16_t stride){ dma_stride = stride; }
    void setBroadcast(bool broadcast){ dma_broadcast = broadcast; }
    void setRequestType(DMARequestType type){ request_type = type; }
    uint16_t base() const { return dma_base; }
    uint16_t len() const { return dma_len; }
    uint16_t stride() const { return dma_stride; }
    bool broadcast() const { return dma_broadcast; }
    DMARequestType requestType() const { return request_type; }

    void issue(DMARequestType type, uint16_t stride, bool broadcast) {
        setRequestType(type);
        setStride(stride);
        setBroadcast(broadcast);
        activate();
    }

    void next();
    void activate();
    bool busy() const { return dma_active; }
private:
    void handle();

    DataMemory* dm;
    Vector* dmrv;
    Vector* dmwv; // static port
    uint16_t dma_base = 0;
    uint16_t dma_offset = 0;
    uint16_t dma_len = 0;
    uint16_t dma_stride = 0;
    bool dma_broadcast = false;
    DMARequestType request_type;
    bool dma_active = false;
    int latency; // 保留 latency 欄位
};

struct PEState {
    InstructionMemory IM;    // 指令記憶體
    DataMemory DM;           // 資料記憶體
    TransformRegFile TR;     // Transform registers
    PsumRegFile PS;          // Psum registers
    DMAController DMA;       // DMA 控制器
    LoopController loops;    // Loop 控制器
    Vector dmrv;             // 最新 DMA 載入值
    Vector dmwv;             // 最新 DMA 寫入值
    VALU valu;               // FP16 統一運算單元

    PEConfig cfg;            // 組態

    uint16_t pc = 0;         // 程式計數器
    bool halted = false;
    uint64_t cycles = 0;
};

} // namespace hybridacc
