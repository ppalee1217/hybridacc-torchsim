#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

SC_MODULE(ALU) {
    sc_core::sc_in<sc_uint<32>> operand_a;
    sc_core::sc_in<sc_uint<32>> operand_b;
    sc_core::sc_in<AluOp> alu_op;
    sc_core::sc_out<sc_uint<32>> result;

    SC_CTOR(ALU) {
        SC_METHOD(compute);
        sensitive << operand_a << operand_b << alu_op;
    }

    void compute() {
        switch (alu_op.read()) {
            /* RV32I Base Integer Instructions */
            case AluOp::ADD:  result.write(operand_a.read() + operand_b.read()); break;
            case AluOp::SUB:  result.write(operand_a.read() - operand_b.read()); break;
            case AluOp::SLL:  result.write(operand_a.read() << (operand_b.read() & 0x1F)); break;
            case AluOp::SLT:  result.write(static_cast<int32_t>(operand_a.read().to_uint()) < static_cast<int32_t>(operand_b.read().to_uint())); break;
            case AluOp::SLTU: result.write(operand_a.read() < operand_b.read()); break;
            case AluOp::XOR:  result.write(operand_a.read() ^ operand_b.read()); break;
            case AluOp::SRL:  result.write(operand_a.read() >> (operand_b.read() & 0x1F)); break;
            case AluOp::SRA:  result.write(static_cast<uint32_t>(static_cast<int32_t>(operand_a.read().to_uint()) >> (operand_b.read() & 0x1F))); break;
            case AluOp::OR:   result.write(operand_a.read() | operand_b.read()); break;
            case AluOp::AND:  result.write(operand_a.read() & operand_b.read()); break;
            /* M-Extension Instructions (Zmmul) */
            case AluOp::MUL:    result.write(static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(operand_a.read().to_uint())) * static_cast<int64_t>(static_cast<int32_t>(operand_b.read().to_uint())))); break;
            case AluOp::MULH:   result.write(static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(operand_a.read().to_uint())) * static_cast<int64_t>(static_cast<int32_t>(operand_b.read().to_uint()))) >> 32)); break;
            case AluOp::MULHSU: result.write(static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(operand_a.read().to_uint())) * static_cast<int64_t>(static_cast<uint64_t>(operand_b.read().to_uint()))) >> 32)); break;
            case AluOp::MULHU:  result.write(static_cast<uint32_t>((static_cast<uint64_t>(operand_a.read().to_uint()) * static_cast<uint64_t>(operand_b.read().to_uint())) >> 32)); break;
            /* Pseudo-ops */
            case AluOp::PASS_B: result.write(operand_b.read()); break;
            case AluOp::ADD_4:  result.write(operand_a.read() + 4); break;
            case AluOp::NOP:
            default:            result.write(0u); break;
        }
    }
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc
