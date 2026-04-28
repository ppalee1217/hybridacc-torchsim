//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   CoreMcu
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Minimal RV32I execution core for bring-up firmware tests.
//                Supports a single-cycle subset sufficient for current
//                top-level boot smoke and basic ALU firmware execution:
//                  * RV32I ALU / branch / jump instructions
//                  * DSRAM lw/sw through the local load-store port
//                  * trap/halt snapshot and IRQ capture
//                Full pipeline behavior is still deferred.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module CoreMcu (
    input  logic        clk,
    input  logic        reset_n,
    input  logic [31:0] boot_addr_i,
    input  logic        core_enable_i,
    input  logic        core_haltreq_i,
    output logic        if_req_valid_o,
    output logic [31:0] if_addr_o,
    input  logic [31:0] if_rdata_i,
    output logic        ls_req_valid_o,
    output logic        ls_req_write_o,
    output logic [31:0] ls_req_addr_o,
    output logic [31:0] ls_req_wdata_o,
    output logic [3:0]  ls_req_wstrb_o,
    input  logic [31:0] ls_resp_rdata_i,
    output logic        mmio_req_valid_o,
    output logic        mmio_req_write_o,
    output logic [31:0] mmio_req_addr_o,
    output logic [31:0] mmio_req_wdata_o,
    output logic [3:0]  mmio_req_wstrb_o,
    input  logic        mmio_resp_valid_i,
    input  logic [31:0] mmio_resp_rdata_i,
    input  logic        irq_meip_i,
    input  logic        irq_msip_i,
    input  logic        irq_mtip_i,
    output logic        retire_valid_o,
    output logic [31:0] retire_pc_o,
    output logic        core_running_o,
    output logic        core_halted_o,
    output logic [31:0] pc_snapshot_o,
    output logic [31:0] cause_snapshot_o
);
    logic [31:0] gpr_reg [0:CORE_GPR_NUM-1];
    logic        booted_reg;
    logic        running_reg;
    logic        halted_reg;
    logic        in_trap_reg;
    logic        wfi_wait_reg;
    logic [31:0] pc_reg;
    logic [31:0] instr_reg;
    logic [63:0] cycle_reg;
    logic [63:0] instret_reg;
    logic [31:0] cause_reg;
    logic [31:0] mstatus_reg;
    logic [31:0] mie_reg;
    logic [31:0] mtvec_reg;
    logic [31:0] mscratch_reg;
    logic [31:0] mepc_reg;
    logic [31:0] mtval_reg;

    logic [31:0] instr_w;
    logic [6:0]  opcode_w;
    logic [2:0]  funct3_w;
    logic [6:0]  funct7_w;
    logic [4:0]  rs1_idx_w;
    logic [4:0]  rs2_idx_w;
    logic [4:0]  rd_idx_w;
    logic [31:0] rs1_data_w;
    logic [31:0] rs2_data_w;
    logic [31:0] imm_i_w;
    logic [31:0] imm_s_w;
    logic [31:0] imm_b_w;
    logic [31:0] imm_u_w;
    logic [31:0] imm_j_w;
    logic [11:0] csr_addr_w;
    logic [31:0] csr_read_data_w;
    logic [31:0] mip_w;
    logic        irq_meip_take_w;
    logic        irq_mtip_take_w;
    logic        irq_msip_take_w;
    logic        is_load_w;
    logic        is_store_w;
    logic [31:0] ls_addr_w;
    logic        load_supported_w;
    logic        store_supported_w;
    logic        is_data_sram_w;
    logic        is_inst_sram_w;
    logic        is_mmio_w;
    logic        mmio_word_access_w;
    logic [31:0] ls_wdata_w;
    logic [3:0]  ls_wstrb_w;
    logic signed [63:0] mul_ss_w;
    logic signed [63:0] mul_su_w;
    logic        [63:0] mul_uu_w;

    function automatic logic [31:0] imm_i_decode(input logic [31:0] instr);
        return {{20{instr[31]}}, instr[31:20]};
    endfunction

    function automatic logic [31:0] imm_s_decode(input logic [31:0] instr);
        return {{20{instr[31]}}, instr[31:25], instr[11:7]};
    endfunction

    function automatic logic [31:0] imm_b_decode(input logic [31:0] instr);
        return {{19{instr[31]}}, instr[31], instr[7], instr[30:25], instr[11:8], 1'b0};
    endfunction

    function automatic logic [31:0] imm_u_decode(input logic [31:0] instr);
        return {instr[31:12], 12'h000};
    endfunction

    function automatic logic [31:0] imm_j_decode(input logic [31:0] instr);
        return {{11{instr[31]}}, instr[31], instr[19:12], instr[20], instr[30:21], 1'b0};
    endfunction

    function automatic logic csr_supported(input logic [11:0] csr_addr);
        unique case (csr_addr)
            CSR_MSTATUS,
            CSR_MISA,
            CSR_MIE,
            CSR_MTVEC,
            CSR_MSCRATCH,
            CSR_MEPC,
            CSR_MCAUSE,
            CSR_MTVAL,
            CSR_MIP,
            CSR_MCYCLE,
            CSR_MINSTRET: return 1'b1;
            default: return 1'b0;
        endcase
    endfunction

    function automatic logic csr_writable(input logic [11:0] csr_addr);
        unique case (csr_addr)
            CSR_MSTATUS,
            CSR_MIE,
            CSR_MTVEC,
            CSR_MSCRATCH,
            CSR_MEPC,
            CSR_MCAUSE,
            CSR_MTVAL,
            CSR_MCYCLE,
            CSR_MINSTRET: return 1'b1;
            default: return 1'b0;
        endcase
    endfunction

    assign instr_w    = if_rdata_i;
    assign opcode_w   = instr_w[6:0];
    assign funct3_w   = instr_w[14:12];
    assign funct7_w   = instr_w[31:25];
    assign rs1_idx_w  = instr_w[19:15];
    assign rs2_idx_w  = instr_w[24:20];
    assign rd_idx_w   = instr_w[11:7];
    assign rs1_data_w = (rs1_idx_w == 5'd0) ? 32'h0 : gpr_reg[rs1_idx_w];
    assign rs2_data_w = (rs2_idx_w == 5'd0) ? 32'h0 : gpr_reg[rs2_idx_w];
    assign imm_i_w    = imm_i_decode(instr_w);
    assign imm_s_w    = imm_s_decode(instr_w);
    assign imm_b_w    = imm_b_decode(instr_w);
    assign imm_u_w    = imm_u_decode(instr_w);
    assign imm_j_w    = imm_j_decode(instr_w);
    assign csr_addr_w = instr_w[31:20];
    assign mip_w = {20'h0, irq_meip_i, 3'h0, irq_mtip_i, 3'h0, irq_msip_i, 3'h0};
    assign irq_meip_take_w = irq_meip_i && mstatus_reg[3] && mie_reg[11] && !in_trap_reg;
    assign irq_mtip_take_w = irq_mtip_i && mstatus_reg[3] && mie_reg[7] && !in_trap_reg;
    assign irq_msip_take_w = irq_msip_i && mstatus_reg[3] && mie_reg[3] && !in_trap_reg;
    assign is_load_w  = (opcode_w == 7'b0000011);
    assign is_store_w = (opcode_w == 7'b0100011);
    assign ls_addr_w  = rs1_data_w + (is_store_w ? imm_s_w : imm_i_w);
    assign is_data_sram_w = (ls_addr_w >= BASE_DATA_RAM) && (ls_addr_w <= END_DATA_RAM);
    assign is_inst_sram_w = (ls_addr_w >= BASE_INST_RAM) && (ls_addr_w <= END_INST_RAM);
    assign is_mmio_w = addr_in_range(ls_addr_w, BASE_LOCAL_CTRL, END_LOCAL_CTRL)
                    || addr_in_range(ls_addr_w, BASE_DMA_MMIO, END_DMA_MMIO)
                    || addr_in_range(ls_addr_w, BASE_LOCAL_TIMER, END_LOCAL_TIMER)
                    || addr_in_range(ls_addr_w, BASE_PLIC, END_PLIC)
                    || addr_in_range(ls_addr_w, BASE_CLUSTER_UNICAST, END_CLUSTER_UNICAST)
                    || addr_in_range(ls_addr_w, BASE_CLUSTER_BCAST, END_CLUSTER_BCAST)
                    || addr_in_range(ls_addr_w, BASE_NLU, END_NLU);
    assign mmio_word_access_w = (funct3_w == 3'b010) && (ls_addr_w[1:0] == 2'b00);
    assign mul_ss_w = $signed({{32{rs1_data_w[31]}}, rs1_data_w}) * $signed({{32{rs2_data_w[31]}}, rs2_data_w});
    assign mul_su_w = $signed({{32{rs1_data_w[31]}}, rs1_data_w}) * $signed({32'h0, rs2_data_w});
    assign mul_uu_w = {32'h0, rs1_data_w} * {32'h0, rs2_data_w};

    always_comb begin
        unique case (csr_addr_w)
            CSR_MSTATUS:  csr_read_data_w = mstatus_reg;
            CSR_MISA:     csr_read_data_w = 32'h4000_1100;
            CSR_MIE:      csr_read_data_w = mie_reg;
            CSR_MTVEC:    csr_read_data_w = mtvec_reg;
            CSR_MSCRATCH: csr_read_data_w = mscratch_reg;
            CSR_MEPC:     csr_read_data_w = mepc_reg;
            CSR_MCAUSE:   csr_read_data_w = cause_reg;
            CSR_MTVAL:    csr_read_data_w = mtval_reg;
            CSR_MIP:      csr_read_data_w = mip_w;
            CSR_MCYCLE:   csr_read_data_w = cycle_reg[31:0];
            CSR_MINSTRET: csr_read_data_w = instret_reg[31:0];
            default:      csr_read_data_w = 32'h0;
        endcase
    end

    always_comb begin
        load_supported_w = 1'b0;
        store_supported_w = 1'b0;
        ls_wdata_w = rs2_data_w;
        ls_wstrb_w = 4'h0;

        unique case (funct3_w)
            3'b000: begin
                load_supported_w = is_load_w;
                store_supported_w = is_store_w;
                ls_wdata_w = rs2_data_w << (8 * ls_addr_w[1:0]);
                ls_wstrb_w = 4'b0001 << ls_addr_w[1:0];
            end
            3'b001: begin
                load_supported_w = is_load_w && (ls_addr_w[0] == 1'b0);
                store_supported_w = is_store_w && (ls_addr_w[0] == 1'b0);
                ls_wdata_w = rs2_data_w << (8 * ls_addr_w[1:0]);
                ls_wstrb_w = ls_addr_w[1] ? 4'b1100 : 4'b0011;
            end
            3'b010: begin
                load_supported_w = is_load_w && (ls_addr_w[1:0] == 2'b00);
                store_supported_w = is_store_w && (ls_addr_w[1:0] == 2'b00);
                ls_wdata_w = rs2_data_w;
                ls_wstrb_w = 4'hF;
            end
            3'b100,
            3'b101: begin
                load_supported_w = is_load_w && ((funct3_w == 3'b100) || ((funct3_w == 3'b101) && (ls_addr_w[0] == 1'b0)));
            end
            default: begin
                load_supported_w = 1'b0;
                store_supported_w = 1'b0;
            end
        endcase
    end

    assign if_req_valid_o = running_reg;
    assign if_addr_o      = pc_reg;

    assign ls_req_valid_o = running_reg && ((is_load_w && load_supported_w && (is_data_sram_w || is_inst_sram_w)) || (is_store_w && store_supported_w && is_data_sram_w));
    assign ls_req_write_o = is_store_w;
    assign ls_req_addr_o  = ls_addr_w;
    assign ls_req_wdata_o = ls_wdata_w;
    assign ls_req_wstrb_o = (running_reg && is_store_w && store_supported_w && is_data_sram_w) ? ls_wstrb_w : 4'h0;

    assign mmio_req_valid_o = running_reg && ((is_load_w && load_supported_w && mmio_word_access_w && is_mmio_w) || (is_store_w && store_supported_w && mmio_word_access_w && is_mmio_w));
    assign mmio_req_write_o = is_store_w;
    assign mmio_req_addr_o  = ls_addr_w;
    assign mmio_req_wdata_o = rs2_data_w;
    assign mmio_req_wstrb_o = (running_reg && is_store_w && store_supported_w && mmio_word_access_w && is_mmio_w) ? 4'hF : 4'h0;

    assign retire_pc_o      = pc_reg;
    assign core_running_o   = running_reg;
    assign core_halted_o    = halted_reg;
    assign pc_snapshot_o    = pc_reg;
    assign cause_snapshot_o = cause_reg;

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int idx = 0; idx < CORE_GPR_NUM; idx++) begin
                gpr_reg[idx] <= 32'h0;
            end
            booted_reg     <= 1'b0;
            running_reg    <= 1'b0;
            halted_reg     <= 1'b1;
            in_trap_reg    <= 1'b0;
            wfi_wait_reg   <= 1'b0;
            pc_reg         <= 32'h0;
            instr_reg      <= 32'h0;
            cycle_reg      <= 64'h0;
            instret_reg    <= 64'h0;
            cause_reg      <= 32'h0;
            mstatus_reg    <= 32'h0;
            mie_reg        <= 32'h0;
            mtvec_reg      <= 32'h0;
            mscratch_reg   <= 32'h0;
            mepc_reg       <= 32'h0;
            mtval_reg      <= 32'h0;
            retire_valid_o <= 1'b0;
        end else begin
            logic [31:0] next_pc;
            logic [31:0] rd_value;
            logic [4:0]  rd_index;
            logic        rd_write_en;
            logic        retire_fire;
            logic        halt_fire;
            logic [31:0] halt_cause;
            logic        branch_taken;
            logic        csr_write_en;
            logic [11:0] csr_write_addr;
            logic [31:0] csr_write_value;
            logic [31:0] csr_src_value;

            cycle_reg <= cycle_reg + 64'd1;
            retire_valid_o <= 1'b0;
            gpr_reg[0] <= 32'h0;

            if (!booted_reg && core_enable_i) begin
                booted_reg  <= 1'b1;
                running_reg <= 1'b1;
                halted_reg  <= 1'b0;
                in_trap_reg <= 1'b0;
                wfi_wait_reg<= 1'b0;
                pc_reg      <= boot_addr_i;
                cause_reg   <= 32'h0;
            end

            if (core_haltreq_i) begin
                running_reg <= 1'b0;
                halted_reg  <= 1'b1;
            end

            if (running_reg) begin
                next_pc     = pc_reg + 32'd4;
                rd_value    = 32'h0;
                rd_index    = rd_idx_w;
                rd_write_en = 1'b0;
                retire_fire = 1'b0;
                halt_fire   = 1'b0;
                halt_cause  = cause_reg;
                branch_taken= 1'b0;
                csr_write_en = 1'b0;
                csr_write_addr = 12'h000;
                csr_write_value = 32'h0;
                csr_src_value = 32'h0;

                instr_reg <= if_rdata_i;

                if (wfi_wait_reg) begin
                    if (irq_meip_take_w || irq_mtip_take_w || irq_msip_take_w) begin
                        wfi_wait_reg <= 1'b0;
                        in_trap_reg <= 1'b1;
                        mepc_reg <= pc_reg + 32'd4;
                        mtval_reg <= 32'h0;
                        mstatus_reg[7] <= mstatus_reg[3];
                        mstatus_reg[3] <= 1'b0;
                        if (irq_meip_take_w)      cause_reg <= 32'h8000_000B;
                        else if (irq_mtip_take_w) cause_reg <= 32'h8000_0007;
                        else                      cause_reg <= 32'h8000_0003;
                        pc_reg <= mtvec_reg;
                    end
                end else begin
                unique case (opcode_w)
                        7'b0110111: begin
                            rd_write_en = 1'b1;
                            rd_value    = imm_u_w;
                            retire_fire = 1'b1;
                        end
                        7'b0010111: begin
                            rd_write_en = 1'b1;
                            rd_value    = pc_reg + imm_u_w;
                            retire_fire = 1'b1;
                        end
                        7'b1101111: begin
                            rd_write_en = 1'b1;
                            rd_value    = pc_reg + 32'd4;
                            next_pc     = pc_reg + imm_j_w;
                            retire_fire = 1'b1;
                        end
                        7'b1100111: begin
                            if (funct3_w == 3'b000) begin
                                rd_write_en = 1'b1;
                                rd_value    = pc_reg + 32'd4;
                                next_pc     = (rs1_data_w + imm_i_w) & ~32'h1;
                                retire_fire = 1'b1;
                            end else begin
                                halt_fire  = 1'b1;
                                halt_cause = 32'd2;
                            end
                        end
                        7'b1100011: begin
                            unique case (funct3_w)
                                3'b000: branch_taken = (rs1_data_w == rs2_data_w);
                                3'b001: branch_taken = (rs1_data_w != rs2_data_w);
                                3'b100: branch_taken = ($signed(rs1_data_w) <  $signed(rs2_data_w));
                                3'b101: branch_taken = ($signed(rs1_data_w) >= $signed(rs2_data_w));
                                3'b110: branch_taken = (rs1_data_w < rs2_data_w);
                                3'b111: branch_taken = (rs1_data_w >= rs2_data_w);
                                default: begin
                                    halt_fire  = 1'b1;
                                    halt_cause = 32'd2;
                                end
                            endcase
                            if (!halt_fire) begin
                                if (branch_taken) begin
                                    next_pc = pc_reg + imm_b_w;
                                end
                                retire_fire = 1'b1;
                            end
                        end
                        7'b0010011: begin
                            unique case (funct3_w)
                                3'b000: rd_value = rs1_data_w + imm_i_w;
                                3'b010: rd_value = ($signed(rs1_data_w) < $signed(imm_i_w)) ? 32'd1 : 32'd0;
                                3'b011: rd_value = (rs1_data_w < imm_i_w) ? 32'd1 : 32'd0;
                                3'b100: rd_value = rs1_data_w ^ imm_i_w;
                                3'b110: rd_value = rs1_data_w | imm_i_w;
                                3'b111: rd_value = rs1_data_w & imm_i_w;
                                3'b001: begin
                                    if (funct7_w == 7'b0000000) rd_value = rs1_data_w << instr_w[24:20];
                                    else begin
                                        halt_fire  = 1'b1;
                                        halt_cause = 32'd2;
                                    end
                                end
                                3'b101: begin
                                    if (funct7_w == 7'b0000000) rd_value = rs1_data_w >> instr_w[24:20];
                                    else if (funct7_w == 7'b0100000) rd_value = $signed(rs1_data_w) >>> instr_w[24:20];
                                    else begin
                                        halt_fire  = 1'b1;
                                        halt_cause = 32'd2;
                                    end
                                end
                                default: begin
                                    halt_fire  = 1'b1;
                                    halt_cause = 32'd2;
                                end
                            endcase
                            if (!halt_fire) begin
                                rd_write_en = 1'b1;
                                retire_fire = 1'b1;
                            end
                        end
                        7'b0110011: begin
                            unique case ({funct7_w, funct3_w})
                                {7'b0000000, 3'b000}: rd_value = rs1_data_w + rs2_data_w;
                                {7'b0100000, 3'b000}: rd_value = rs1_data_w - rs2_data_w;
                                {7'b0000001, 3'b000}: rd_value = mul_uu_w[31:0];
                                {7'b0000001, 3'b001}: rd_value = mul_ss_w[63:32];
                                {7'b0000001, 3'b010}: rd_value = mul_su_w[63:32];
                                {7'b0000001, 3'b011}: rd_value = mul_uu_w[63:32];
                                {7'b0000000, 3'b001}: rd_value = rs1_data_w << rs2_data_w[4:0];
                                {7'b0000000, 3'b010}: rd_value = ($signed(rs1_data_w) < $signed(rs2_data_w)) ? 32'd1 : 32'd0;
                                {7'b0000000, 3'b011}: rd_value = (rs1_data_w < rs2_data_w) ? 32'd1 : 32'd0;
                                {7'b0000000, 3'b100}: rd_value = rs1_data_w ^ rs2_data_w;
                                {7'b0000000, 3'b101}: rd_value = rs1_data_w >> rs2_data_w[4:0];
                                {7'b0100000, 3'b101}: rd_value = $signed(rs1_data_w) >>> rs2_data_w[4:0];
                                {7'b0000000, 3'b110}: rd_value = rs1_data_w | rs2_data_w;
                                {7'b0000000, 3'b111}: rd_value = rs1_data_w & rs2_data_w;
                                default: begin
                                    halt_fire  = 1'b1;
                                    halt_cause = 32'd2;
                                end
                            endcase
                            if (!halt_fire) begin
                                rd_write_en = 1'b1;
                                retire_fire = 1'b1;
                            end
                        end
                        7'b0000011: begin
                            if (load_supported_w && (is_data_sram_w || is_inst_sram_w)) begin
                                rd_write_en = 1'b1;
                                unique case (funct3_w)
                                    3'b000: rd_value = {{24{ls_resp_rdata_i[ls_addr_w[1:0]*8 + 7]}}, ls_resp_rdata_i[ls_addr_w[1:0]*8 +: 8]};
                                    3'b001: rd_value = {{16{ls_resp_rdata_i[ls_addr_w[1]*16 + 15]}}, ls_resp_rdata_i[ls_addr_w[1]*16 +: 16]};
                                    3'b010: rd_value = ls_resp_rdata_i;
                                    3'b100: rd_value = {24'h0, ls_resp_rdata_i[ls_addr_w[1:0]*8 +: 8]};
                                    3'b101: rd_value = {16'h0, ls_resp_rdata_i[ls_addr_w[1]*16 +: 16]};
                                    default: rd_value = 32'h0;
                                endcase
                                retire_fire = 1'b1;
                            end else if (load_supported_w && mmio_word_access_w && is_mmio_w) begin
                                if (mmio_resp_valid_i) begin
                                    rd_write_en = 1'b1;
                                    rd_value    = mmio_resp_rdata_i;
                                    retire_fire = 1'b1;
                                end
                            end else begin
                                halt_fire  = 1'b1;
                                halt_cause = 32'd2;
                            end
                        end
                        7'b0100011: begin
                            if (store_supported_w && is_data_sram_w) begin
                                retire_fire = 1'b1;
                            end else if (store_supported_w && mmio_word_access_w && is_mmio_w) begin
                                if (mmio_resp_valid_i) begin
                                    retire_fire = 1'b1;
                                end
                            end else begin
                                halt_fire  = 1'b1;
                                halt_cause = 32'd2;
                            end
                        end
                        7'b1110011: begin
                            if (instr_w == 32'h0000_0073) begin
                                in_trap_reg <= 1'b1;
                                mepc_reg <= pc_reg;
                                mtval_reg <= 32'h0;
                                mstatus_reg[7] <= mstatus_reg[3];
                                mstatus_reg[3] <= 1'b0;
                                cause_reg <= 32'd11;
                                pc_reg <= mtvec_reg;
                            end else if (instr_w == 32'h0010_0073) begin
                                halt_fire  = 1'b1;
                                halt_cause = 32'd3;
                            end else if (instr_w == 32'h1050_0073) begin
                                wfi_wait_reg <= 1'b1;
                            end else if (instr_w == 32'h3020_0073) begin
                                in_trap_reg <= 1'b0;
                                mstatus_reg[3] <= mstatus_reg[7];
                                mstatus_reg[7] <= 1'b1;
                                next_pc = mepc_reg;
                                retire_fire = 1'b1;
                            end else if (funct3_w != 3'b000) begin
                                if (!csr_supported(csr_addr_w)) begin
                                    halt_fire  = 1'b1;
                                    halt_cause = 32'd2;
                                end else begin
                                    rd_write_en = 1'b1;
                                    rd_value    = csr_read_data_w;
                                    csr_write_addr = csr_addr_w;
                                    unique case (funct3_w)
                                        3'b001: begin
                                            csr_write_en = 1'b1;
                                            csr_write_value = rs1_data_w;
                                        end
                                        3'b010: begin
                                            csr_src_value = rs1_data_w;
                                            if (rs1_idx_w != 5'd0) begin
                                                csr_write_en = 1'b1;
                                                csr_write_value = csr_read_data_w | csr_src_value;
                                            end
                                        end
                                        3'b011: begin
                                            csr_src_value = rs1_data_w;
                                            if (rs1_idx_w != 5'd0) begin
                                                csr_write_en = 1'b1;
                                                csr_write_value = csr_read_data_w & ~csr_src_value;
                                            end
                                        end
                                        3'b101: begin
                                            csr_write_en = 1'b1;
                                            csr_write_value = {27'h0, rs1_idx_w};
                                        end
                                        3'b110: begin
                                            csr_src_value = {27'h0, rs1_idx_w};
                                            if (rs1_idx_w != 5'd0) begin
                                                csr_write_en = 1'b1;
                                                csr_write_value = csr_read_data_w | csr_src_value;
                                            end
                                        end
                                        3'b111: begin
                                            csr_src_value = {27'h0, rs1_idx_w};
                                            if (rs1_idx_w != 5'd0) begin
                                                csr_write_en = 1'b1;
                                                csr_write_value = csr_read_data_w & ~csr_src_value;
                                            end
                                        end
                                        default: begin
                                            halt_fire  = 1'b1;
                                            halt_cause = 32'd2;
                                        end
                                    endcase
                                    if (!halt_fire && csr_write_en && !csr_writable(csr_write_addr)) begin
                                        halt_fire  = 1'b1;
                                        halt_cause = 32'd2;
                                    end
                                    if (!halt_fire) begin
                                        retire_fire = 1'b1;
                                    end
                                end
                            end else begin
                                halt_fire  = 1'b1;
                                halt_cause = 32'd2;
                            end
                        end
                        default: begin
                            halt_fire  = 1'b1;
                            halt_cause = 32'd2;
                        end
                    endcase

                if (halt_fire) begin
                    running_reg <= 1'b0;
                    halted_reg  <= 1'b1;
                    cause_reg   <= halt_cause;
                end else if (retire_fire && (irq_meip_take_w || irq_mtip_take_w || irq_msip_take_w)) begin
                    if (rd_write_en && (rd_index != 5'd0)) begin
                        gpr_reg[rd_index] <= rd_value;
                    end
                    if (csr_write_en) begin
                        unique case (csr_write_addr)
                            CSR_MSTATUS:  mstatus_reg  <= csr_write_value;
                            CSR_MIE:      mie_reg      <= csr_write_value;
                            CSR_MTVEC:    mtvec_reg    <= csr_write_value;
                            CSR_MSCRATCH: mscratch_reg <= csr_write_value;
                            CSR_MEPC:     mepc_reg     <= csr_write_value;
                            CSR_MCAUSE:   cause_reg    <= csr_write_value;
                            CSR_MTVAL:    mtval_reg    <= csr_write_value;
                            CSR_MCYCLE:   cycle_reg    <= {32'h0, csr_write_value};
                            CSR_MINSTRET: instret_reg  <= {32'h0, csr_write_value};
                            default: ;
                        endcase
                    end
                    in_trap_reg <= 1'b1;
                    mepc_reg <= next_pc;
                    mtval_reg <= 32'h0;
                    mstatus_reg[7] <= mstatus_reg[3];
                    mstatus_reg[3] <= 1'b0;
                    if (irq_meip_take_w)      cause_reg <= 32'h8000_000B;
                    else if (irq_mtip_take_w) cause_reg <= 32'h8000_0007;
                    else                      cause_reg <= 32'h8000_0003;
                    pc_reg <= mtvec_reg;
                    instret_reg <= instret_reg + 64'd1;
                    retire_valid_o <= 1'b1;
                end else begin
                    if (rd_write_en && (rd_index != 5'd0)) begin
                        gpr_reg[rd_index] <= rd_value;
                    end
                    if (csr_write_en) begin
                        unique case (csr_write_addr)
                            CSR_MSTATUS:  mstatus_reg  <= csr_write_value;
                            CSR_MIE:      mie_reg      <= csr_write_value;
                            CSR_MTVEC:    mtvec_reg    <= csr_write_value;
                            CSR_MSCRATCH: mscratch_reg <= csr_write_value;
                            CSR_MEPC:     mepc_reg     <= csr_write_value;
                            CSR_MCAUSE:   cause_reg    <= csr_write_value;
                            CSR_MTVAL:    mtval_reg    <= csr_write_value;
                            CSR_MCYCLE:   cycle_reg    <= {32'h0, csr_write_value};
                            CSR_MINSTRET: instret_reg  <= {32'h0, csr_write_value};
                            default: ;
                        endcase
                    end
                    if (retire_fire) begin
                        pc_reg <= next_pc;
                        instret_reg <= instret_reg + 64'd1;
                        retire_valid_o <= 1'b1;
                    end
                end
                end
            end
        end
    end

endmodule