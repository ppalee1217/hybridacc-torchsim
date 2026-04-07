#pragma once

/**
 * @file PipelineTypes.hpp
 * @brief Inter-stage pipeline register types for the cc_core_mcu 5-stage
 *        RV32I_Zmmul_Zicsr pipeline.
 *
 * Each struct represents the architectural / micro-architectural payload
 * carried by one pipeline register.  Every struct provides operator==,
 * operator<< and sc_trace for waveform debug.
 */

#include <systemc>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

// ============================================================================
// Basic widths merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kRvPcBit = 32;
static constexpr uint32_t kRvMemAddrBitWidth = 32;
static constexpr uint32_t kRvMemDataBitWidth = 32;
static constexpr uint32_t kRvDataBitWidth = 32;
static constexpr uint32_t kRvDataBits = 32;
static constexpr uint32_t kRvDataSize = 32;

// ============================================================================
// Cache-related constants merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kCacheBlockBits = 2;
static constexpr uint32_t kCacheIndexBits = 5;
static constexpr uint32_t kCacheTagBits = 23;
static constexpr uint32_t kCacheDataBits = 128;
static constexpr uint32_t kCacheLines = 1u << kCacheIndexBits;
static constexpr uint32_t kCacheWriteBits = 16;
static constexpr uint32_t kCacheTypeBits = 3;
static constexpr uint32_t kCacheTypeByte = 0b000;
static constexpr uint32_t kCacheTypeHalf = 0b001;
static constexpr uint32_t kCacheTypeWord = 0b010;
static constexpr uint32_t kCacheTypeByteUnsigned = 0b100;
static constexpr uint32_t kCacheTypeHalfUnsigned = 0b101;

// ============================================================================
// RV32I instruction-field helpers
// ============================================================================

/// @brief Extract opcode[6:0]
static inline uint32_t rv_opcode(uint32_t inst) { return inst & 0x7F; }
/// @brief Extract rd[11:7]
static inline uint32_t rv_rd(uint32_t inst)     { return (inst >> 7) & 0x1F; }
/// @brief Extract funct3[14:12]
static inline uint32_t rv_funct3(uint32_t inst)  { return (inst >> 12) & 0x7; }
/// @brief Extract rs1[19:15]
static inline uint32_t rv_rs1(uint32_t inst)     { return (inst >> 15) & 0x1F; }
/// @brief Extract rs2[24:20]
static inline uint32_t rv_rs2(uint32_t inst)     { return (inst >> 20) & 0x1F; }
/// @brief Extract funct7[31:25]
static inline uint32_t rv_funct7(uint32_t inst)  { return (inst >> 25) & 0x7F; }

// ============================================================================
// RV32I opcode constants
// ============================================================================

static constexpr uint32_t kOpLui      = 0b0110111;
static constexpr uint32_t kOpAuipc    = 0b0010111;
static constexpr uint32_t kOpJal      = 0b1101111;
static constexpr uint32_t kOpJalr     = 0b1100111;
static constexpr uint32_t kOpBranch   = 0b1100011;
static constexpr uint32_t kOpLoad     = 0b0000011;
static constexpr uint32_t kOpStore    = 0b0100011;
static constexpr uint32_t kOpOpImm    = 0b0010011;
static constexpr uint32_t kOpOp       = 0b0110011;
static constexpr uint32_t kOpFence    = 0b0001111;
static constexpr uint32_t kOpSystem   = 0b1110011;
static constexpr uint32_t kOpFlw      = 0b0000111;
static constexpr uint32_t kOpFsw      = 0b0100111;
static constexpr uint32_t kOpFop      = 0b1010011;
static constexpr uint32_t kOpFmaddS   = 0b1000011;
static constexpr uint32_t kOpFmsubS   = 0b1000111;
static constexpr uint32_t kOpFnmsubS  = 0b1001011;
static constexpr uint32_t kOpFnmaddS  = 0b1001111;

// ============================================================================
// Branch condition encoding merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kBranchEq  = 0b000;
static constexpr uint32_t kBranchNe  = 0b001;
static constexpr uint32_t kBranchLt  = 0b100;
static constexpr uint32_t kBranchGe  = 0b101;
static constexpr uint32_t kBranchLtu = 0b110;
static constexpr uint32_t kBranchGeu = 0b111;

// ============================================================================
// ALU / M-extension encodings merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kAluFunct3AddSub = 0b000;
static constexpr uint32_t kAluFunct3Sll    = 0b001;
static constexpr uint32_t kAluFunct3Slt    = 0b010;
static constexpr uint32_t kAluFunct3Sltu   = 0b011;
static constexpr uint32_t kAluFunct3Xor    = 0b100;
static constexpr uint32_t kAluFunct3SrlSra = 0b101;
static constexpr uint32_t kAluFunct3Or     = 0b110;
static constexpr uint32_t kAluFunct3And    = 0b111;

static constexpr uint32_t kFunct7Base      = 0b0000000;
static constexpr uint32_t kFunct7SubSra    = 0b0100000;
static constexpr uint32_t kFunct7MulDiv    = 0b0000001;

static constexpr uint32_t kMulFunct3Mul    = 0b000;
static constexpr uint32_t kMulFunct3Mulh   = 0b001;
static constexpr uint32_t kMulFunct3Mulhsu = 0b010;
static constexpr uint32_t kMulFunct3Mulhu  = 0b011;
static constexpr uint32_t kMulFunct3Div    = 0b100;
static constexpr uint32_t kMulFunct3Divu   = 0b101;
static constexpr uint32_t kMulFunct3Rem    = 0b110;
static constexpr uint32_t kMulFunct3Remu   = 0b111;

// ============================================================================
// F-extension encodings merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kFunct5FaddS    = 0b00000;
static constexpr uint32_t kFunct5FsubS    = 0b00001;
static constexpr uint32_t kFunct5FmulS    = 0b00010;
static constexpr uint32_t kFunct5FdivS    = 0b00011;
static constexpr uint32_t kFunct5FsgnjS   = 0b00100;
static constexpr uint32_t kFunct5FminS    = 0b00101;
static constexpr uint32_t kFunct5FsqrtS   = 0b01011;
static constexpr uint32_t kFunct5Fcompare = 0b10100;
static constexpr uint32_t kFunct5FcvtWS   = 0b11000;
static constexpr uint32_t kFunct5FcvtSW   = 0b11010;
static constexpr uint32_t kFunct5FmvXW    = 0b11100;
static constexpr uint32_t kFunct5FmvWX    = 0b11110;

static constexpr uint32_t kFunct3RoundDyn = 0b111;
static constexpr uint32_t kFunct3Fsgnj    = 0b000;
static constexpr uint32_t kFunct3Fsgnjn   = 0b001;
static constexpr uint32_t kFunct3Fsgnjx   = 0b010;
static constexpr uint32_t kFunct3Fmin     = 0b000;
static constexpr uint32_t kFunct3Fmax     = 0b001;
static constexpr uint32_t kFunct3Fle      = 0b000;
static constexpr uint32_t kFunct3Flt      = 0b001;
static constexpr uint32_t kFunct3Feq      = 0b010;
static constexpr uint32_t kFunct3Fclass   = 0b001;
static constexpr uint32_t kFunct3FmvXW    = 0b000;
static constexpr uint32_t kFunct3FmvWX    = 0b000;

// ============================================================================
// Load/store width and CSR funct3 constants merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kMemWidthByte  = 0b000;
static constexpr uint32_t kMemWidthHalf  = 0b001;
static constexpr uint32_t kMemWidthWord  = 0b010;
static constexpr uint32_t kMemWidthUByte = 0b100;
static constexpr uint32_t kMemWidthUHalf = 0b101;

static constexpr uint32_t kCsrFunct3Csrrw  = 0b001;
static constexpr uint32_t kCsrFunct3Csrrs  = 0b010;
static constexpr uint32_t kCsrFunct3Csrrc  = 0b011;
static constexpr uint32_t kCsrFunct3Csrrwi = 0b101;
static constexpr uint32_t kCsrFunct3Csrrsi = 0b110;
static constexpr uint32_t kCsrFunct3Csrrci = 0b111;

// ============================================================================
// Control select fields merged from rv32_define.svh
// ============================================================================

enum class ExecuteOutSel : uint8_t {
    ALU_OUT = 0,
    MALU_OUT = 1,
    FPU_OUT = 2,
    CSR_OUT = 3,
};

enum class ExecuteOp1Sel : uint8_t {
    RS1 = 0,
    PC = 1,
    ZERO = 2,
};

enum class ExecuteOp2Sel : uint8_t {
    RS2 = 0,
    IMM = 1,
};

enum class WriteBackSel : uint8_t {
    PC_PLUS_4 = 0,
    ALUOUT = 1,
    LD_DATA = 2,
};

static constexpr uint32_t kCtrlWbSelLsb = 15;
static constexpr uint32_t kCtrlMemR = 14;
static constexpr uint32_t kCtrlMemW = 13;
static constexpr uint32_t kCtrlExeJ = 12;
static constexpr uint32_t kCtrlExeB = 11;
static constexpr uint32_t kCtrlExeOp1SelLsb = 9;
static constexpr uint32_t kCtrlExeOp2Sel = 8;
static constexpr uint32_t kCtrlExeOutSelLsb = 6;
static constexpr uint32_t kCtrlRs1 = 5;
static constexpr uint32_t kCtrlRs2 = 4;
static constexpr uint32_t kCtrlRd = 3;
static constexpr uint32_t kCtrlFrs1 = 2;
static constexpr uint32_t kCtrlFrs2 = 1;
static constexpr uint32_t kCtrlFrd = 0;

// ============================================================================
// Interrupt / CSR constants merged from rv32_define.svh
// ============================================================================

static constexpr uint32_t kInterruptTimeBit = 7;
static constexpr uint32_t kInterruptExternalBit = 11;
static constexpr uint32_t kEcallMcause = 11;
static constexpr uint32_t kEbreakMcause = 3;
static constexpr uint32_t kMstatusMieBit = 3;
static constexpr uint32_t kMstatusMpieBit = 7;
static constexpr uint32_t kMtvecModeDirect = 0b00;
static constexpr uint32_t kMtvecModeVectored = 0b01;
static constexpr uint32_t kMstatusMask = 0x00001888u;
static constexpr uint32_t kMieMask = 0x00000880u;

static constexpr uint32_t kCsrAddrMvendorid = 0xF11;
static constexpr uint32_t kCsrAddrMarchid = 0xF12;
static constexpr uint32_t kCsrAddrMimpid = 0xF13;
static constexpr uint32_t kCsrAddrMhartid = 0xF14;
static constexpr uint32_t kCsrAddrMstatus = 0x300;
static constexpr uint32_t kCsrAddrMvisa = 0x301;
static constexpr uint32_t kCsrAddrMedeleg = 0x302;
static constexpr uint32_t kCsrAddrMideleg = 0x303;
static constexpr uint32_t kCsrAddrMie = 0x304;
static constexpr uint32_t kCsrAddrMtvec = 0x305;
static constexpr uint32_t kCsrAddrMcounteren = 0x306;
static constexpr uint32_t kCsrAddrMscratch = 0x340;
static constexpr uint32_t kCsrAddrMepc = 0x341;
static constexpr uint32_t kCsrAddrMcause = 0x342;
static constexpr uint32_t kCsrAddrMtval = 0x343;
static constexpr uint32_t kCsrAddrMip = 0x344;
static constexpr uint32_t kCsrAddrMcycle = 0xC00;
static constexpr uint32_t kCsrAddrMinstret = 0xC02;
static constexpr uint32_t kCsrAddrMcycleh = 0xC80;
static constexpr uint32_t kCsrAddrMinstreth = 0xC82;

// ============================================================================
// Pipeline constants merged from rv32_define.svh
// ============================================================================

static constexpr uint64_t kDivStageList = 0x1000100010001ULL;
static constexpr uint32_t kDivStageCnt = 5;

// ============================================================================
// Helper utilities for packed control words and float bit-cast
// ============================================================================

template <typename T>
static inline bool bit_test(T value, unsigned bit) {
    return ((static_cast<uint64_t>(value) >> bit) & 0x1ULL) != 0ULL;
}

static inline bool ctrl_bit(const sc_uint<17>& ctrl, unsigned bit) {
    return ctrl[bit];
}

static inline uint32_t ctrl_field(const sc_uint<17>& ctrl, unsigned lsb, unsigned width) {
    return ctrl.range(lsb + width - 1, lsb).to_uint();
}

static inline sc_uint<17> pack_control_sel(
    WriteBackSel wb_sel,
    bool mem_read,
    bool mem_write,
    bool exe_jump,
    bool exe_branch,
    ExecuteOp1Sel op1_sel,
    ExecuteOp2Sel op2_sel,
    ExecuteOutSel out_sel,
    bool rs1,
    bool rs2,
    bool rd,
    bool frs1,
    bool frs2,
    bool frd) {
    sc_uint<17> value = 0;
    value.range(kCtrlWbSelLsb + 1, kCtrlWbSelLsb) = static_cast<uint32_t>(wb_sel);
    value[kCtrlMemR] = mem_read;
    value[kCtrlMemW] = mem_write;
    value[kCtrlExeJ] = exe_jump;
    value[kCtrlExeB] = exe_branch;
    value.range(kCtrlExeOp1SelLsb + 1, kCtrlExeOp1SelLsb) = static_cast<uint32_t>(op1_sel);
    value[kCtrlExeOp2Sel] = static_cast<uint32_t>(op2_sel);
    value.range(kCtrlExeOutSelLsb + 1, kCtrlExeOutSelLsb) = static_cast<uint32_t>(out_sel);
    value[kCtrlRs1] = rs1;
    value[kCtrlRs2] = rs2;
    value[kCtrlRd] = rd;
    value[kCtrlFrs1] = frs1;
    value[kCtrlFrs2] = frs2;
    value[kCtrlFrd] = frd;
    return value;
}

static inline float bits_to_float(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline uint32_t float_to_bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// ============================================================================
// ALU operation encoding
// ============================================================================

enum class AluOp : uint8_t {
    ADD = 0, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND,
    MUL, MULH, MULHSU, MULHU,
    PASS_B,    ///< passthrough operand B (for LUI)
    ADD_4,     ///< PC+4 (for JAL/JALR link)
    NOP,
};

inline std::ostream& operator<<(std::ostream& os, AluOp op) {
    static const char* names[] = {
        "ADD","SUB","SLL","SLT","SLTU","XOR","SRL","SRA","OR","AND",
        "MUL","MULH","MULHSU","MULHU","PASS_B","ADD_4","NOP"
    };
    return os << names[static_cast<int>(op)];
}

inline void sc_trace(sc_trace_file* tf, const AluOp& op, const std::string& name) {
    sc_trace(tf, static_cast<uint8_t>(op), name);
}

// ============================================================================
// Branch comparison encoding
// ============================================================================

enum class BranchOp : uint8_t {
    NONE = 0, BEQ, BNE, BLT, BGE, BLTU, BGEU,
};

inline std::ostream& operator<<(std::ostream& os, BranchOp op) {
    static const char* names[] = {"NONE","BEQ","BNE","BLT","BGE","BLTU","BGEU"};
    return os << names[static_cast<int>(op)];
}

inline void sc_trace(sc_trace_file* tf, const BranchOp& op, const std::string& name) {
    sc_trace(tf, static_cast<uint8_t>(op), name);
}

// ============================================================================
// CSR operation encoding
// ============================================================================

enum class CsrOp : uint8_t {
    NONE = 0, CSRRW, CSRRS, CSRRC, CSRRWI, CSRRSI, CSRRCI,
};

inline std::ostream& operator<<(std::ostream& os, CsrOp op) {
    static const char* names[] = {"NONE","CSRRW","CSRRS","CSRRC","CSRRWI","CSRRSI","CSRRCI"};
    return os << names[static_cast<int>(op)];
}

inline void sc_trace(sc_trace_file* tf, const CsrOp& op, const std::string& name) {
    sc_trace(tf, static_cast<uint8_t>(op), name);
}

// ============================================================================
// Memory access size encoding
// ============================================================================

enum class MemSize : uint8_t {
    BYTE = 0, HALF = 1, WORD = 2,
};

inline std::ostream& operator<<(std::ostream& os, MemSize s) {
    static const char* names[] = {"BYTE","HALF","WORD"};
    return os << names[static_cast<int>(s)];
}

inline void sc_trace(sc_trace_file* tf, const MemSize& s, const std::string& name) {
    sc_trace(tf, static_cast<uint8_t>(s), name);
}

// ============================================================================
// IF/ID pipeline register
// ============================================================================

struct IfIdReg {
    uint32_t pc;        ///< program counter of this instruction
    uint32_t inst;      ///< raw 32-bit instruction word
    bool     valid;     ///< instruction is valid (not bubble)

    bool operator==(const IfIdReg& o) const {
        return pc == o.pc && inst == o.inst && valid == o.valid;
    }
    friend std::ostream& operator<<(std::ostream& os, const IfIdReg& r) {
        os << "IF/ID{pc=0x" << std::hex << r.pc
           << ", inst=0x" << r.inst << std::dec
           << ", v=" << r.valid << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const IfIdReg& r, const std::string& name) {
        sc_trace(tf, r.pc,    name + ".pc");
        sc_trace(tf, r.inst,  name + ".inst");
        sc_trace(tf, r.valid, name + ".valid");
    }
};

// ============================================================================
// ID/EX pipeline register
// ============================================================================

struct IdExReg {
    uint32_t pc;
    uint32_t inst;     ///< raw 32-bit instruction word (for debug trace)
    bool     valid;

    // Decoded control signals
    AluOp    alu_op;
    BranchOp branch_op;
    CsrOp    csr_op;
    uint32_t csr_addr;

    // Register file read results / immediates
    uint32_t rs1_data;
    uint32_t rs2_data;
    uint32_t imm;
    uint32_t rd_idx;       ///< destination register index
    uint32_t rs1_idx;
    uint32_t rs2_idx;

    // Memory control
    bool     mem_read;
    bool     mem_write;
    MemSize  mem_size;
    bool     mem_unsigned; ///< zero-extend load

    // Writeback control
    bool     wb_enable;
    bool     use_alu;      ///< 1=ALU result, 0=mem/csr
    bool     is_jump;      ///< JAL or JALR
    bool     is_jalr;
    bool     is_auipc;
    bool     is_lui;
    bool     is_ebreak;    ///< EBREAK instruction (halt request)

    bool operator==(const IdExReg& o) const {
        return pc == o.pc && inst == o.inst && valid == o.valid && alu_op == o.alu_op &&
               branch_op == o.branch_op && rd_idx == o.rd_idx;
    }
    friend std::ostream& operator<<(std::ostream& os, const IdExReg& r) {
        os << "ID/EX{pc=0x" << std::hex << r.pc
           << ", inst=0x" << r.inst << std::dec
           << ", v=" << r.valid
           << ", alu=" << r.alu_op
           << ", br=" << r.branch_op
           << ", rd=" << r.rd_idx << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const IdExReg& r, const std::string& name) {
        sc_trace(tf, r.pc,        name + ".pc");
        sc_trace(tf, r.inst,      name + ".inst");
        sc_trace(tf, r.valid,     name + ".valid");
        sc_trace(tf, r.alu_op,    name + ".alu_op");
        sc_trace(tf, r.branch_op, name + ".branch_op");
        sc_trace(tf, r.rd_idx,    name + ".rd_idx");
        sc_trace(tf, r.mem_read,  name + ".mem_read");
        sc_trace(tf, r.mem_write, name + ".mem_write");
        sc_trace(tf, r.wb_enable, name + ".wb_enable");
    }
};

// ============================================================================
// EX/MEM pipeline register
// ============================================================================

struct ExMemReg {
    uint32_t pc;
    bool     valid;

    uint32_t alu_result;
    uint32_t rs2_data;     ///< store data (forwarded)
    uint32_t rd_idx;

    // Memory control
    bool     mem_read;
    bool     mem_write;
    MemSize  mem_size;
    bool     mem_unsigned;

    // Writeback control
    bool     wb_enable;

    // CSR result (if CSR instruction)
    uint32_t csr_rdata;
    bool     csr_written;

    // MMIO flag (address >= MMIO region)
    bool     is_mmio;

    bool operator==(const ExMemReg& o) const {
        return pc == o.pc && valid == o.valid && alu_result == o.alu_result &&
               rd_idx == o.rd_idx;
    }
    friend std::ostream& operator<<(std::ostream& os, const ExMemReg& r) {
        os << "EX/MEM{pc=0x" << std::hex << r.pc
           << ", alu=0x" << r.alu_result << std::dec
           << ", v=" << r.valid
           << ", rd=" << r.rd_idx
           << ", mr=" << r.mem_read
           << ", mw=" << r.mem_write << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const ExMemReg& r, const std::string& name) {
        sc_trace(tf, r.pc,         name + ".pc");
        sc_trace(tf, r.valid,      name + ".valid");
        sc_trace(tf, r.alu_result, name + ".alu_result");
        sc_trace(tf, r.rd_idx,     name + ".rd_idx");
        sc_trace(tf, r.mem_read,   name + ".mem_read");
        sc_trace(tf, r.mem_write,  name + ".mem_write");
        sc_trace(tf, r.is_mmio,    name + ".is_mmio");
    }
};

// ============================================================================
// MEM/WB pipeline register
// ============================================================================

struct MemWbReg {
    uint32_t pc;
    bool     valid;

    uint32_t wb_data;     ///< data to write back (ALU, mem load, or CSR)
    uint32_t rd_idx;
    bool     wb_enable;

    bool operator==(const MemWbReg& o) const {
        return pc == o.pc && valid == o.valid && wb_data == o.wb_data &&
               rd_idx == o.rd_idx;
    }
    friend std::ostream& operator<<(std::ostream& os, const MemWbReg& r) {
        os << "MEM/WB{pc=0x" << std::hex << r.pc
           << ", wb=0x" << r.wb_data << std::dec
           << ", v=" << r.valid
           << ", rd=" << r.rd_idx
           << ", we=" << r.wb_enable << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const MemWbReg& r, const std::string& name) {
        sc_trace(tf, r.pc,        name + ".pc");
        sc_trace(tf, r.valid,     name + ".valid");
        sc_trace(tf, r.wb_data,   name + ".wb_data");
        sc_trace(tf, r.rd_idx,    name + ".rd_idx");
        sc_trace(tf, r.wb_enable, name + ".wb_enable");
    }
};

// ============================================================================
// Hazard / bypass control signals  (combinational between stages)
// ============================================================================

struct HazardCtrl {
    bool stall_if;      ///< stall IF stage
    bool stall_id;      ///< stall ID stage
    bool flush_if;      ///< flush IF (branch mispredict / trap)
    bool flush_id;      ///< flush ID (branch mispredict / trap)
    bool flush_ex;      ///< flush EX (branch mispredict / trap)

    bool operator==(const HazardCtrl& o) const {
        return stall_if == o.stall_if && stall_id == o.stall_id &&
               flush_if == o.flush_if && flush_id == o.flush_id &&
               flush_ex == o.flush_ex;
    }
    friend std::ostream& operator<<(std::ostream& os, const HazardCtrl& h) {
        os << "Hazard{sIF=" << h.stall_if << ", sID=" << h.stall_id
           << ", fIF=" << h.flush_if << ", fID=" << h.flush_id
           << ", fEX=" << h.flush_ex << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const HazardCtrl& h, const std::string& name) {
        sc_trace(tf, h.stall_if, name + ".stall_if");
        sc_trace(tf, h.stall_id, name + ".stall_id");
        sc_trace(tf, h.flush_if, name + ".flush_if");
        sc_trace(tf, h.flush_id, name + ".flush_id");
        sc_trace(tf, h.flush_ex, name + ".flush_ex");
    }
};

// ============================================================================
// Bypass (forwarding) data  (EX->ID, MEM->ID)
// ============================================================================

struct BypassData {
    bool     valid;
    uint32_t rd_idx;
    uint32_t data;

    bool operator==(const BypassData& o) const {
        return valid == o.valid && rd_idx == o.rd_idx && data == o.data;
    }
    friend std::ostream& operator<<(std::ostream& os, const BypassData& b) {
        os << "Bypass{v=" << b.valid << ", rd=" << b.rd_idx
           << ", d=0x" << std::hex << b.data << std::dec << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const BypassData& b, const std::string& name) {
        sc_trace(tf, b.valid,  name + ".valid");
        sc_trace(tf, b.rd_idx, name + ".rd_idx");
        sc_trace(tf, b.data,   name + ".data");
    }
};

} // namespace core
} // namespace hybridacc
