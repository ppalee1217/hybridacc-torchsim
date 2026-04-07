#pragma once

#include <systemc>
#include <cstdint>
#include "Core/rv32i_mcu/PipelineTypes.hpp"
#include "Core/rv32i_mcu/component/ALU.hpp"
#include "Core/rv32i_mcu/component/CSR.hpp"

namespace hybridacc {
namespace core {
namespace rv32i_mcu {

template <unsigned BITWIDTH = 32>
SC_MODULE(ExecuteStage) {
    sc_core::sc_in<bool> clk;
    sc_core::sc_in<bool> rst_n;

    sc_core::sc_in<bool> timer_interrupt;
    sc_core::sc_in<bool> external_interrupt;
    sc_core::sc_in<bool> software_interrupt;
    sc_core::sc_out<bool> stall_wfi;

    sc_core::sc_out<bool> stall_DH;
    sc_core::sc_out<bool> stall_FP;
    sc_core::sc_out<bool> stall_M;
    sc_core::sc_out<bool> stall_fence;
    sc_core::sc_in<bool> stall;

    sc_core::sc_in<sc_uint<kRvPcBit>> IF_pc;
    sc_core::sc_in<sc_uint<kRvPcBit>> IF_pc_plus_4;
    sc_core::sc_out<bool> EXE_taken;
    sc_core::sc_out<bool> EXE_update;
    sc_core::sc_out<bool> EXE_bp_miss;
    sc_core::sc_out<sc_uint<kRvPcBit>> EXE_pc_target;

    sc_core::sc_in<sc_uint<kRvPcBit>> ID_pc;
    sc_core::sc_in<sc_uint<kRvPcBit>> ID_pc_plus_4;
    sc_core::sc_in<sc_uint<BITWIDTH>> ID_inst;
    sc_core::sc_in<sc_uint<BITWIDTH>> ID_rs1_data;
    sc_core::sc_in<sc_uint<BITWIDTH>> ID_rs2_data;
    sc_core::sc_in<sc_uint<BITWIDTH>> ID_imm;
    sc_core::sc_in<sc_uint<17>> ID_controll_sel;
    sc_core::sc_in<bool> ID_inst_valid;

    sc_core::sc_out<sc_uint<kRvPcBit>> EXE_pc;
    sc_core::sc_out<sc_uint<kRvPcBit>> EXE_pc_plus_4;
    sc_core::sc_out<bool> EXE_mem_R;
    sc_core::sc_out<bool> EXE_mem_W;
    sc_core::sc_out<sc_uint<5>> EXE_rf_w_index;
    sc_core::sc_out<bool> EXE_irf_wen;
    sc_core::sc_out<bool> EXE_frf_wen;
    sc_core::sc_out<sc_uint<3>> EXE_funct3;
    sc_core::sc_out<sc_uint<2>> EXE_wb_sel;
    sc_core::sc_out<sc_uint<BITWIDTH>> EXE_store_data;
    sc_core::sc_out<sc_uint<BITWIDTH>> EXE_exe_out;
    sc_core::sc_out<bool> EXE_inst_valid;

    sc_core::sc_in<bool> MEM_irf_wen;
    sc_core::sc_in<bool> MEM_frf_wen;
    sc_core::sc_in<sc_uint<5>> MEM_rf_w_index;
    sc_core::sc_in<sc_uint<BITWIDTH>> MEM_rf_w_data;
    sc_core::sc_in<bool> dm_valid;

    sc_core::sc_in<bool> WB_irf_wen;
    sc_core::sc_in<bool> WB_frf_wen;
    sc_core::sc_in<sc_uint<5>> WB_rf_w_index;
    sc_core::sc_in<sc_uint<BITWIDTH>> WB_rf_w_data;

    sc_core::sc_signal<sc_uint<kRvPcBit>> exe_pc_reg_;
    sc_core::sc_signal<sc_uint<kRvPcBit>> exe_pc_plus_4_reg_;
    sc_core::sc_signal<sc_uint<BITWIDTH>> exe_inst_reg_;
    sc_core::sc_signal<sc_uint<BITWIDTH>> exe_rs1_data_reg_;
    sc_core::sc_signal<sc_uint<BITWIDTH>> exe_rs2_data_reg_;
    sc_core::sc_signal<sc_uint<BITWIDTH>> exe_imm_reg_;
    sc_core::sc_signal<sc_uint<17>> exe_control_sel_reg_;
    sc_core::sc_signal<bool> exe_inst_valid_reg_;
    sc_core::sc_signal<bool> stall_fence_reg_;
    sc_core::sc_signal<bool> interrupt_reg_;

    sc_core::sc_signal<sc_uint<32>> alu_operand_a_sig_;
    sc_core::sc_signal<sc_uint<32>> alu_operand_b_sig_;
    sc_core::sc_signal<AluOp> alu_op_sig_;
    sc_core::sc_signal<sc_uint<32>> alu_result_sig_;

    sc_core::sc_signal<bool> csr_inst_cnt_sig_;
    sc_core::sc_signal<CsrOp> csr_op_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_imm_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_rs1_sig_;
    sc_core::sc_signal<sc_uint<12>> csr_addr_sig_;
    sc_core::sc_signal<bool> csr_rs1_valid_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_out_sig_;
    sc_core::sc_signal<bool> csr_interrupt_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_mtvec_out_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_mepc_out_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_pc_sig_;
    sc_core::sc_signal<sc_uint<32>> csr_pc_plus_4_sig_;
    sc_core::sc_signal<bool> csr_wfi_sig_;
    sc_core::sc_signal<bool> csr_ecall_sig_;
    sc_core::sc_signal<bool> csr_ebreak_sig_;
    sc_core::sc_signal<bool> csr_mret_sig_;

    ALU alu_0_;
    CSR csr_0_;

    SC_CTOR(ExecuteStage)
        : alu_0_("ALU_0"),
          csr_0_("CSR_0") {
        alu_0_.operand_a(alu_operand_a_sig_);
        alu_0_.operand_b(alu_operand_b_sig_);
        alu_0_.alu_op(alu_op_sig_);
        alu_0_.result(alu_result_sig_);

        csr_0_.clk(clk);
        csr_0_.rst_n(rst_n);
        csr_0_.inst_cnt(csr_inst_cnt_sig_);
        csr_0_.exe_funct3(csr_op_sig_);
        csr_0_.exe_imm(csr_imm_sig_);
        csr_0_.exe_rs1(csr_rs1_sig_);
        csr_0_.csr_imm(csr_addr_sig_);
        csr_0_.rs1_idx_valid(csr_rs1_valid_sig_);
        csr_0_.csr_out(csr_out_sig_);
        csr_0_.timer_interrupt(timer_interrupt);
        csr_0_.external_interrupt(external_interrupt);
        csr_0_.software_interrupt(software_interrupt);
        csr_0_.interrupt(csr_interrupt_sig_);
        csr_0_.mtvec_out(csr_mtvec_out_sig_);
        csr_0_.mepc_out(csr_mepc_out_sig_);
        csr_0_.pc(csr_pc_sig_);
        csr_0_.pc_plus_4(csr_pc_plus_4_sig_);
        csr_0_.wfi(csr_wfi_sig_);
        csr_0_.ecall(csr_ecall_sig_);
        csr_0_.ebreak(csr_ebreak_sig_);
        csr_0_.mret(csr_mret_sig_);
        csr_0_.stall(stall);

        SC_METHOD(drive_submodule_inputs);
        sensitive << stall << IF_pc << IF_pc_plus_4 << ID_pc << ID_pc_plus_4 << ID_inst << ID_inst_valid
                  << exe_pc_reg_ << exe_pc_plus_4_reg_ << exe_inst_reg_ << exe_rs1_data_reg_ << exe_rs2_data_reg_ << exe_imm_reg_
                  << exe_control_sel_reg_ << exe_inst_valid_reg_ << interrupt_reg_
                  << MEM_irf_wen << MEM_rf_w_index << MEM_rf_w_data
                  << WB_irf_wen << WB_rf_w_index << WB_rf_w_data
                  << csr_interrupt_sig_;

        SC_METHOD(compute_comb);
        sensitive << stall << IF_pc << IF_pc_plus_4 << ID_pc << ID_pc_plus_4 << ID_inst << ID_inst_valid << ID_controll_sel
                  << exe_pc_reg_ << exe_pc_plus_4_reg_ << exe_inst_reg_ << exe_imm_reg_ << exe_control_sel_reg_ << exe_inst_valid_reg_
                  << stall_fence_reg_ << interrupt_reg_
                  << MEM_irf_wen << MEM_frf_wen << MEM_rf_w_index << MEM_rf_w_data
                  << WB_irf_wen << WB_frf_wen << WB_rf_w_index << WB_rf_w_data
                  << alu_result_sig_ << csr_interrupt_sig_ << csr_mtvec_out_sig_ << csr_mepc_out_sig_ << csr_out_sig_;

        SC_METHOD(write_ff);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

    static CsrOp decode_csr_op(uint32_t opcode, uint32_t funct3) {
        if (opcode != kOpSystem) return CsrOp::NONE;
        switch (funct3) {
            case kCsrFunct3Csrrw: return CsrOp::CSRRW;
            case kCsrFunct3Csrrs: return CsrOp::CSRRS;
            case kCsrFunct3Csrrc: return CsrOp::CSRRC;
            case kCsrFunct3Csrrwi: return CsrOp::CSRRWI;
            case kCsrFunct3Csrrsi: return CsrOp::CSRRSI;
            case kCsrFunct3Csrrci: return CsrOp::CSRRCI;
            default: return CsrOp::NONE;
        }
    }

    static AluOp decode_alu_op(uint32_t opcode, uint32_t funct3, uint32_t funct7) {
        switch (opcode) {
            case kOpLui:
                return AluOp::PASS_B;
            case kOpOpImm:
                switch (funct3) {
                    case kAluFunct3AddSub: return AluOp::ADD;
                    case kAluFunct3Sll: return AluOp::SLL;
                    case kAluFunct3Slt: return AluOp::SLT;
                    case kAluFunct3Sltu: return AluOp::SLTU;
                    case kAluFunct3Xor: return AluOp::XOR;
                    case kAluFunct3SrlSra: return (funct7 == kFunct7SubSra) ? AluOp::SRA : AluOp::SRL;
                    case kAluFunct3Or: return AluOp::OR;
                    case kAluFunct3And: return AluOp::AND;
                    default: return AluOp::NOP;
                }
            case kOpOp:
                if (funct7 == kFunct7MulDiv) {
                    switch (funct3) {
                        case kMulFunct3Mul: return AluOp::MUL;
                        case kMulFunct3Mulh: return AluOp::MULH;
                        case kMulFunct3Mulhsu: return AluOp::MULHSU;
                        case kMulFunct3Mulhu: return AluOp::MULHU;
                        default: return AluOp::NOP;
                    }
                }
                switch (funct3) {
                    case kAluFunct3AddSub: return (funct7 == kFunct7SubSra) ? AluOp::SUB : AluOp::ADD;
                    case kAluFunct3Sll: return AluOp::SLL;
                    case kAluFunct3Slt: return AluOp::SLT;
                    case kAluFunct3Sltu: return AluOp::SLTU;
                    case kAluFunct3Xor: return AluOp::XOR;
                    case kAluFunct3SrlSra: return (funct7 == kFunct7SubSra) ? AluOp::SRA : AluOp::SRL;
                    case kAluFunct3Or: return AluOp::OR;
                    case kAluFunct3And: return AluOp::AND;
                    default: return AluOp::NOP;
                }
            case kOpBranch:
            case kOpJal:
            case kOpJalr:
            case kOpLoad:
            case kOpStore:
            case kOpAuipc:
                return AluOp::ADD;
            default:
                return AluOp::ADD;
        }
    }

    uint32_t exe_inst_value() const {
        return exe_inst_reg_.read().to_uint();
    }

    uint32_t exe_rs1_forwarded(uint32_t rs1, const sc_uint<17>& exe_ctrl) const {
        const bool rs1_mem_forward = (rs1 == MEM_rf_w_index.read().to_uint() && rs1 != 0u) && exe_ctrl[kCtrlRs1] && MEM_irf_wen.read();
        const bool rs1_wb_forward = (rs1 == WB_rf_w_index.read().to_uint() && rs1 != 0u) && exe_ctrl[kCtrlRs1] && WB_irf_wen.read();
        if (rs1_mem_forward) return MEM_rf_w_data.read().to_uint();
        if (rs1_wb_forward) return WB_rf_w_data.read().to_uint();
        return exe_rs1_data_reg_.read().to_uint();
    }

    uint32_t exe_rs2_forwarded(uint32_t rs2, const sc_uint<17>& exe_ctrl) const {
        const bool rs2_mem_forward = (rs2 == MEM_rf_w_index.read().to_uint() && rs2 != 0u) && exe_ctrl[kCtrlRs2] && MEM_irf_wen.read();
        const bool rs2_wb_forward = (rs2 == WB_rf_w_index.read().to_uint() && rs2 != 0u) && exe_ctrl[kCtrlRs2] && WB_irf_wen.read();
        if (rs2_mem_forward) return MEM_rf_w_data.read().to_uint();
        if (rs2_wb_forward) return WB_rf_w_data.read().to_uint();
        return exe_rs2_data_reg_.read().to_uint();
    }

    static bool branch_taken(uint32_t funct3, uint32_t lhs, uint32_t rhs) {
        switch (funct3) {
            case kBranchEq: return lhs == rhs;
            case kBranchNe: return lhs != rhs;
            case kBranchLt: return static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs);
            case kBranchGe: return !(static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs));
            case kBranchLtu: return lhs < rhs;
            case kBranchGeu: return !(lhs < rhs);
            default: return false;
        }
    }

    void drive_submodule_inputs() {
        const uint32_t inst = exe_inst_value();
        const sc_uint<17> exe_ctrl = exe_control_sel_reg_.read();
        const uint32_t opcode = inst & 0x7Fu;
        const uint32_t funct3 = (inst >> 12) & 0x7u;
        const uint32_t funct7 = ((opcode == kOpOp) || (opcode == kOpOpImm && funct3 == kAluFunct3SrlSra))
            ? ((inst >> 25) & 0x7Fu)
            : 0u;
        const uint32_t rs1 = (inst >> 15) & 0x1Fu;
        const uint32_t rs2 = (inst >> 20) & 0x1Fu;
        const uint32_t exe_rs1 = exe_rs1_forwarded(rs1, exe_ctrl);
        const uint32_t exe_rs2 = exe_rs2_forwarded(rs2, exe_ctrl);

        uint32_t operand1 = 0u;
        switch (ctrl_field(exe_ctrl, kCtrlExeOp1SelLsb, 2)) {
            case static_cast<uint32_t>(ExecuteOp1Sel::RS1): operand1 = exe_rs1; break;
            case static_cast<uint32_t>(ExecuteOp1Sel::PC): operand1 = exe_pc_reg_.read().to_uint(); break;
            case static_cast<uint32_t>(ExecuteOp1Sel::ZERO):
            default: operand1 = 0u; break;
        }

        const uint32_t operand2 = exe_ctrl[kCtrlExeOp2Sel] ? exe_imm_reg_.read().to_uint() : exe_rs2;
        const bool mret = inst == 0x30200073u;
        const bool wfi = inst == 0x10500073u;
        const bool ecall = inst == 0x00000073u;
        const bool ebreak = inst == 0x00100073u;

        alu_operand_a_sig_.write(operand1);
        alu_operand_b_sig_.write(operand2);
        alu_op_sig_.write(decode_alu_op(opcode, funct3, funct7));

        csr_inst_cnt_sig_.write(exe_inst_valid_reg_.read() && !stall.read());
        csr_op_sig_.write(decode_csr_op(opcode, funct3));
        csr_imm_sig_.write(exe_imm_reg_.read());
        csr_rs1_sig_.write(exe_rs1);
        csr_addr_sig_.write((inst >> 20) & 0xFFFu);
        csr_rs1_valid_sig_.write(true);
        csr_pc_sig_.write(exe_inst_valid_reg_.read() ? exe_pc_reg_.read() : (ID_inst_valid.read() ? ID_pc.read() : IF_pc.read()));
        csr_pc_plus_4_sig_.write(exe_inst_valid_reg_.read() ? exe_pc_plus_4_reg_.read() : (ID_inst_valid.read() ? ID_pc_plus_4.read() : IF_pc_plus_4.read()));
        csr_wfi_sig_.write(wfi);
        csr_ecall_sig_.write(ecall);
        csr_ebreak_sig_.write(ebreak);
        csr_mret_sig_.write(mret);
    }

    void compute_comb() {
        const uint32_t inst = exe_inst_value();
        const sc_uint<17> exe_ctrl = exe_control_sel_reg_.read();
        const uint32_t opcode = inst & 0x7Fu;
        const uint32_t rd = (inst >> 7) & 0x1Fu;
        const uint32_t funct3 = (inst >> 12) & 0x7u;
        const uint32_t rs1 = (inst >> 15) & 0x1Fu;
        const uint32_t rs2 = (inst >> 20) & 0x1Fu;
        const uint32_t id_rs1 = (ID_inst.read().to_uint() >> 15) & 0x1Fu;
        const uint32_t id_rs2 = (ID_inst.read().to_uint() >> 20) & 0x1Fu;
        const uint32_t id_pred = (ID_inst.read().to_uint() >> 24) & 0xFu;
        const bool pred_r_finished = bit_test(id_pred, 0) && dm_valid.read();
        const bool pred_w_finished = bit_test(id_pred, 1) && dm_valid.read();

        const uint32_t exe_rs1 = exe_rs1_forwarded(rs1, exe_ctrl);
        const uint32_t exe_rs2 = exe_rs2_forwarded(rs2, exe_ctrl);
        const bool mret = inst == 0x30200073u;
        const bool wfi = inst == 0x10500073u;
        const bool interrupt_latched = csr_interrupt_sig_.read() || interrupt_reg_.read();

        stall_wfi.write(wfi && !interrupt_latched);
        stall_FP.write(false);
        stall_M.write(false);
        stall_fence.write(stall_fence_reg_.read());

        EXE_pc.write(exe_pc_reg_.read());
        EXE_pc_plus_4.write(exe_pc_plus_4_reg_.read());
        EXE_funct3.write(funct3);
        EXE_rf_w_index.write(rd);
        EXE_irf_wen.write(exe_ctrl[kCtrlRd]);
        EXE_frf_wen.write(false);
        EXE_mem_R.write(exe_ctrl[kCtrlMemR]);
        EXE_mem_W.write(exe_ctrl[kCtrlMemW]);
        EXE_wb_sel.write(ctrl_field(exe_ctrl, kCtrlWbSelLsb, 2));
        EXE_inst_valid.write(exe_inst_valid_reg_.read());

        const bool dh_rs1 = (id_rs1 == rd && id_rs1 != 0u) && ID_controll_sel.read()[kCtrlRs1] && exe_ctrl[kCtrlRd];
        const bool dh_rs2 = (id_rs2 == rd && id_rs2 != 0u) && ID_controll_sel.read()[kCtrlRs2] && exe_ctrl[kCtrlRd];
        stall_DH.write(exe_ctrl[kCtrlMemR] && (dh_rs1 || dh_rs2));

        const bool branch = branch_taken(funct3, exe_rs1, exe_rs2);
        const uint32_t alu_out = alu_result_sig_.read().to_uint();

        uint32_t exe_out = 0u;
        switch (ctrl_field(exe_ctrl, kCtrlExeOutSelLsb, 2)) {
            case static_cast<uint32_t>(ExecuteOutSel::ALU_OUT):
            case static_cast<uint32_t>(ExecuteOutSel::MALU_OUT):
                exe_out = alu_out;
                break;
            case static_cast<uint32_t>(ExecuteOutSel::CSR_OUT):
                exe_out = csr_out_sig_.read().to_uint();
                break;
            case static_cast<uint32_t>(ExecuteOutSel::FPU_OUT):
            default:
                exe_out = 0u;
                break;
        }

        EXE_exe_out.write(exe_out);
        EXE_store_data.write(exe_rs2);

        const bool exe_update = (!stall.read()) && exe_inst_valid_reg_.read() && (exe_ctrl[kCtrlExeB] || exe_ctrl[kCtrlExeJ]);
        const bool exe_taken = (exe_inst_valid_reg_.read() && branch && exe_ctrl[kCtrlExeB]) || exe_ctrl[kCtrlExeJ];
        EXE_update.write(exe_update);
        EXE_taken.write(exe_taken);

        bool exe_bp_miss = false;
        uint32_t exe_pc_target = exe_pc_plus_4_reg_.read().to_uint();
        if (mret) {
            exe_pc_target = csr_mepc_out_sig_.read().to_uint();
            exe_bp_miss = true;
        } else if (interrupt_latched) {
            exe_pc_target = csr_mtvec_out_sig_.read().to_uint();
            exe_bp_miss = true;
        } else if (exe_taken) {
            exe_bp_miss = (alu_out == ID_pc.read().to_uint()) ? false : ((exe_ctrl[kCtrlExeB] || exe_ctrl[kCtrlExeJ]) && exe_inst_valid_reg_.read());
            exe_pc_target = alu_out;
        } else {
            exe_bp_miss = (exe_pc_plus_4_reg_.read().to_uint() == ID_pc.read().to_uint()) ? false : ((exe_ctrl[kCtrlExeB] || exe_ctrl[kCtrlExeJ]) && exe_inst_valid_reg_.read());
            exe_pc_target = exe_pc_plus_4_reg_.read().to_uint();
        }
        EXE_bp_miss.write(exe_bp_miss);
        EXE_pc_target.write(exe_pc_target);

        (void)MEM_frf_wen.read();
        (void)WB_frf_wen.read();
        (void)opcode;
        (void)pred_r_finished;
        (void)pred_w_finished;
    }

    void write_ff() {
        if (!rst_n.read()) {
            exe_pc_reg_.write(0u);
            exe_pc_plus_4_reg_.write(0u);
            exe_inst_reg_.write(0u);
            exe_rs1_data_reg_.write(0u);
            exe_rs2_data_reg_.write(0u);
            exe_imm_reg_.write(0u);
            exe_control_sel_reg_.write(0u);
            exe_inst_valid_reg_.write(false);
            stall_fence_reg_.write(false);
            interrupt_reg_.write(false);
        } else {
            const uint32_t id_opcode = ID_inst.read().to_uint() & 0x7Fu;
            const bool next_stall_dh = stall_DH.read();
            const bool interrupt_latched = csr_interrupt_sig_.read() || interrupt_reg_.read();

            exe_pc_reg_.write(stall.read() ? exe_pc_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<kRvPcBit>(0u) : ID_pc.read()));
            exe_pc_plus_4_reg_.write(stall.read() ? exe_pc_plus_4_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<kRvPcBit>(0u) : ID_pc_plus_4.read()));
            exe_inst_reg_.write(stall.read() ? exe_inst_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<BITWIDTH>(0u) : ID_inst.read()));
            exe_rs1_data_reg_.write(stall.read() ? exe_rs1_data_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<BITWIDTH>(0u) : ID_rs1_data.read()));
            exe_rs2_data_reg_.write(stall.read() ? exe_rs2_data_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<BITWIDTH>(0u) : ID_rs2_data.read()));
            exe_imm_reg_.write(stall.read() ? exe_imm_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<BITWIDTH>(0u) : ID_imm.read()));
            exe_control_sel_reg_.write(stall.read() ? exe_control_sel_reg_.read() : ((next_stall_dh || !ID_inst_valid.read()) ? sc_uint<17>(0u) : ID_controll_sel.read()));
            exe_inst_valid_reg_.write(stall.read() ? exe_inst_valid_reg_.read() : ((next_stall_dh || interrupt_latched) ? false : ID_inst_valid.read()));

            if (stall.read()) {
                interrupt_reg_.write(csr_interrupt_sig_.read() ? true : interrupt_reg_.read());
            } else {
                interrupt_reg_.write(false);
            }

            const uint32_t id_pred = (ID_inst.read().to_uint() >> 24) & 0xFu;
            const bool pred_r_finished = bit_test(id_pred, 0) && dm_valid.read();
            const bool pred_w_finished = bit_test(id_pred, 1) && dm_valid.read();
            stall_fence_reg_.write((stall_fence_reg_.read() && (pred_r_finished || pred_w_finished))
                ? false
                : (ID_inst_valid.read() && (id_opcode == kOpFence)));
        }
    }
};

} // namespace rv32i_mcu
} // namespace core
} // namespace hybridacc