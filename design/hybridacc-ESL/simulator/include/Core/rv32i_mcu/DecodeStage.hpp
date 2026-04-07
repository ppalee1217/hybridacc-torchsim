#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"
#include "Core/rv32i_mcu/component/GPR.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

template <unsigned BITWIDTH = 32, unsigned REG_SIZE = 32, unsigned INDEX_BITS = 5>
SC_MODULE(DecodeStage) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;

    sc_core::sc_in<bool> stall;
    sc_core::sc_in<bool> flush;

    sc_core::sc_in<sc_uint<kRvPcBit>> IF_pc;
    sc_core::sc_in<sc_uint<kRvPcBit>> IF_pc_plus_4;
    sc_core::sc_in<sc_uint<kRvMemDataBitWidth>> IF_inst;
    sc_core::sc_in<bool> IF_inst_valid;

    sc_core::sc_out<sc_uint<kRvPcBit>> ID_pc;
    sc_core::sc_out<sc_uint<kRvPcBit>> ID_pc_plus_4;
    sc_core::sc_out<sc_uint<BITWIDTH>> ID_inst;
    sc_core::sc_out<sc_uint<BITWIDTH>> ID_rs1_data;
    sc_core::sc_out<sc_uint<BITWIDTH>> ID_rs2_data;
    sc_core::sc_out<sc_uint<BITWIDTH>> ID_imm;
    sc_core::sc_out<sc_uint<17>> ID_controll_sel;
    sc_core::sc_out<bool> ID_inst_valid;

    sc_core::sc_in<bool> WB_irf_wen;
    sc_core::sc_in<bool> WB_frf_wen;
    sc_core::sc_in<sc_uint<INDEX_BITS>> WB_rf_w_index;
    sc_core::sc_in<sc_uint<BITWIDTH>> WB_rf_w_data;

    sc_core::sc_signal<sc_uint<kRvPcBit>> id_pc_reg_;
    sc_core::sc_signal<sc_uint<kRvPcBit>> id_pc_plus_4_reg_;
    sc_core::sc_signal<sc_uint<BITWIDTH>> id_inst_reg_;
    sc_core::sc_signal<bool> id_inst_valid_reg_;

    sc_core::sc_signal<bool> gpr_write_en_sig_;
    sc_core::sc_signal<sc_uint<5>> gpr_write_idx_sig_;
    sc_core::sc_signal<sc_uint<32>> gpr_write_data_sig_;
    sc_core::sc_signal<sc_uint<5>> gpr_read_idx1_sig_;
    sc_core::sc_signal<sc_uint<32>> gpr_read_data1_sig_;
    sc_core::sc_signal<sc_uint<5>> gpr_read_idx2_sig_;
    sc_core::sc_signal<sc_uint<32>> gpr_read_data2_sig_;

    GPR gpr_0_;

    SC_CTOR(DecodeStage)
        : gpr_0_("GPR_0") {
        gpr_0_.clk(clk);
        gpr_0_.rst_n(rst_n);
        gpr_0_.write_en(gpr_write_en_sig_);
        gpr_0_.write_idx(gpr_write_idx_sig_);
        gpr_0_.write_data(gpr_write_data_sig_);
        gpr_0_.read_idx1(gpr_read_idx1_sig_);
        gpr_0_.read_data1(gpr_read_data1_sig_);
        gpr_0_.read_idx2(gpr_read_idx2_sig_);
        gpr_0_.read_data2(gpr_read_data2_sig_);

        SC_METHOD(drive_gpr_ports);
        sensitive << flush << id_inst_reg_ << WB_irf_wen << WB_rf_w_index << WB_rf_w_data;

        SC_METHOD(compute_comb);
        sensitive << flush << id_pc_reg_ << id_pc_plus_4_reg_ << id_inst_reg_ << id_inst_valid_reg_
                  << gpr_read_data1_sig_ << gpr_read_data2_sig_
                  << WB_irf_wen << WB_rf_w_index << WB_rf_w_data;

        SC_METHOD(write_ff);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

    void drive_gpr_ports() {
        const sc_uint<BITWIDTH> inst = flush.read() ? sc_uint<BITWIDTH>(0u) : id_inst_reg_.read();
        gpr_write_en_sig_.write(WB_irf_wen.read());
        gpr_write_idx_sig_.write(WB_rf_w_index.read());
        gpr_write_data_sig_.write(WB_rf_w_data.read());
        gpr_read_idx1_sig_.write(inst.range(19, 15));
        gpr_read_idx2_sig_.write(inst.range(24, 20));
    }

    void compute_comb() {
        const sc_uint<BITWIDTH> inst = flush.read() ? sc_uint<BITWIDTH>(0u) : id_inst_reg_.read();
        const bool inst_valid = flush.read() ? false : id_inst_valid_reg_.read();

        const uint32_t opcode = inst.range(6, 0).to_uint();
        const uint32_t funct3 = inst.range(14, 12).to_uint();
        const uint32_t rs1 = inst.range(19, 15).to_uint();
        const uint32_t rs2 = inst.range(24, 20).to_uint();
        const uint32_t funct7 = inst.range(31, 25).to_uint();

        sc_uint<BITWIDTH> imm = 0;
        switch (opcode) {
            case kOpOpImm:
                if (funct3 == kAluFunct3Sll || funct3 == kAluFunct3SrlSra) {
                    imm = inst.range(24, 20);
                } else {
                    imm = static_cast<uint32_t>(static_cast<int32_t>(inst.to_uint()) >> 20);
                }
                break;
            case kOpJalr:
            case kOpLoad:
                imm = static_cast<uint32_t>(static_cast<int32_t>(inst.to_uint()) >> 20);
                break;
            case kOpSystem:
                imm = rs1;
                break;
            case kOpStore: {
                uint32_t value = ((inst >> 7) & 0x1Fu) | ((inst >> 25) << 5);
                if (bit_test(inst.to_uint(), 31)) value |= 0xFFFFF000u;
                imm = value;
                break;
            }
            case kOpAuipc:
            case kOpLui:
                imm = inst & 0xFFFFF000u;
                break;
            case kOpBranch: {
                uint32_t value = ((inst >> 7) & 0x1Eu) |
                                 ((inst >> 20) & 0x7E0u) |
                                 ((inst << 4) & 0x800u) |
                                 ((inst >> 19) & 0x1000u);
                if (bit_test(inst.to_uint(), 31)) value |= 0xFFFFE000u;
                imm = value;
                break;
            }
            case kOpJal: {
                uint32_t value = ((inst >> 20) & 0x7FEu) |
                                 ((inst >> 9) & 0x800u) |
                                 (inst & 0xFF000u) |
                                 ((inst >> 11) & 0x100000u);
                if (bit_test(inst.to_uint(), 31)) value |= 0xFFE00000u;
                imm = value;
                break;
            }
            default:
                imm = 0u;
                break;
        }

        sc_uint<17> control_sel = pack_control_sel(
            WriteBackSel::ALUOUT,
            false,
            false,
            false,
            false,
            ExecuteOp1Sel::RS1,
            ExecuteOp2Sel::RS2,
            ExecuteOutSel::ALU_OUT,
            false,
            false,
            false,
            false,
            false,
            false);

        switch (opcode) {
            case kOpLoad:
                control_sel = pack_control_sel(WriteBackSel::LD_DATA, true, false, false, false,
                                               ExecuteOp1Sel::RS1, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               true, false, true, false, false, false);
                break;
            case kOpStore:
                control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, true, false, false,
                                               ExecuteOp1Sel::RS1, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               true, true, false, false, false, false);
                break;
            case kOpBranch:
                control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, true,
                                               ExecuteOp1Sel::PC, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               true, true, false, false, false, false);
                break;
            case kOpJalr:
                control_sel = pack_control_sel(WriteBackSel::PC_PLUS_4, false, false, true, false,
                                               ExecuteOp1Sel::RS1, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               true, false, true, false, false, false);
                break;
            case kOpJal:
                control_sel = pack_control_sel(WriteBackSel::PC_PLUS_4, false, false, true, false,
                                               ExecuteOp1Sel::PC, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               false, false, true, false, false, false);
                break;
            case kOpOpImm:
                control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, false,
                                               ExecuteOp1Sel::RS1, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               true, false, true, false, false, false);
                break;
            case kOpOp:
                if (funct7 == kFunct7MulDiv && funct3 <= kMulFunct3Mulhu) {
                    control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, false,
                                                   ExecuteOp1Sel::RS1, ExecuteOp2Sel::RS2, ExecuteOutSel::MALU_OUT,
                                                   true, true, true, false, false, false);
                } else if (funct7 != kFunct7MulDiv) {
                    control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, false,
                                                   ExecuteOp1Sel::RS1, ExecuteOp2Sel::RS2, ExecuteOutSel::ALU_OUT,
                                                   true, true, true, false, false, false);
                }
                break;
            case kOpAuipc:
                control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, false,
                                               ExecuteOp1Sel::PC, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               false, false, true, false, false, false);
                break;
            case kOpLui:
                control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, false,
                                               ExecuteOp1Sel::ZERO, ExecuteOp2Sel::IMM, ExecuteOutSel::ALU_OUT,
                                               false, false, true, false, false, false);
                break;
            case kOpSystem:
                control_sel = pack_control_sel(WriteBackSel::ALUOUT, false, false, false, false,
                                               ExecuteOp1Sel::RS1, ExecuteOp2Sel::RS2, ExecuteOutSel::CSR_OUT,
                                               true, false, true, false, false, false);
                break;
            default:
                break;
        }

        const sc_uint<BITWIDTH> rs1_data_mux = gpr_read_data1_sig_.read();
        const sc_uint<BITWIDTH> rs2_data_mux = gpr_read_data2_sig_.read();

        const bool rs1_forward = (rs1 == WB_rf_w_index.read().to_uint() && rs1 != 0u) && control_sel[kCtrlRs1] && WB_irf_wen.read();
        const bool rs2_forward = (rs2 == WB_rf_w_index.read().to_uint() && rs2 != 0u) && control_sel[kCtrlRs2] && WB_irf_wen.read();

        ID_pc.write(id_pc_reg_.read());
        ID_pc_plus_4.write(id_pc_plus_4_reg_.read());
        ID_inst.write(inst);
        ID_inst_valid.write(inst_valid);
        ID_imm.write(imm);
        ID_controll_sel.write(control_sel);
        ID_rs1_data.write(rs1_forward ? WB_rf_w_data.read() : rs1_data_mux);
        ID_rs2_data.write(rs2_forward ? WB_rf_w_data.read() : rs2_data_mux);

        (void)WB_frf_wen.read();
    }

    void write_ff() {
        if (!rst_n.read()) {
            id_pc_reg_.write(0u);
            id_pc_plus_4_reg_.write(0u);
            id_inst_reg_.write(0u);
            id_inst_valid_reg_.write(false);
        } else {
            id_pc_reg_.write(stall.read() ? id_pc_reg_.read() : (flush.read() ? sc_uint<kRvPcBit>(0u) : IF_pc.read()));
            id_pc_plus_4_reg_.write(stall.read() ? id_pc_plus_4_reg_.read() : (flush.read() ? sc_uint<kRvPcBit>(0u) : IF_pc_plus_4.read()));
            id_inst_reg_.write(stall.read() ? id_inst_reg_.read() : (flush.read() ? sc_uint<BITWIDTH>(0u) : IF_inst.read()));
            id_inst_valid_reg_.write(stall.read() ? id_inst_valid_reg_.read() : (flush.read() ? false : IF_inst_valid.read()));
        }
    }
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc