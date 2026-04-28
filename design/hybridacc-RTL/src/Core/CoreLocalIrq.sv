//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   CoreLocalIrq
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Local MSIP/MTIP timer block for CoreController baseline.
// Dependencies:  src/Core/core_pkg.sv
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
import core_pkg::*;

module CoreLocalIrq (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        mmio_req_valid_i,
    input  logic        mmio_req_write_i,
    input  logic [31:0] mmio_req_addr_i,
    input  logic [31:0] mmio_req_wdata_i,
    output logic        mmio_resp_valid_o,
    output logic [31:0] mmio_resp_rdata_o,
    output logic        irq_msip_o,
    output logic        irq_mtip_o
);
    logic        msip_reg;
    logic [31:0] mtimecmp_lo_reg;
    logic [31:0] mtimecmp_hi_reg;
    logic [31:0] mtime_lo_reg;
    logic [31:0] mtime_hi_reg;
    logic        timer_en_reg;

    assign irq_msip_o = msip_reg;
    assign irq_mtip_o = timer_en_reg && ({mtime_hi_reg, mtime_lo_reg} >= {mtimecmp_hi_reg, mtimecmp_lo_reg});

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            msip_reg          <= 1'b0;
            mtimecmp_lo_reg   <= 32'hFFFF_FFFF;
            mtimecmp_hi_reg   <= 32'hFFFF_FFFF;
            mtime_lo_reg      <= 32'h0;
            mtime_hi_reg      <= 32'h0;
            timer_en_reg      <= 1'b0;
            mmio_resp_valid_o <= 1'b0;
            mmio_resp_rdata_o <= 32'h0;
        end else begin
            mmio_resp_valid_o <= 1'b0;
            if (timer_en_reg) begin
                {mtime_hi_reg, mtime_lo_reg} <= {mtime_hi_reg, mtime_lo_reg} + 64'd1;
            end

            if (mmio_req_valid_i) begin
                mmio_resp_valid_o <= 1'b1;
                if (mmio_req_write_i) begin
                    unique case (mmio_req_addr_i)
                        TIMER_MSIP:        msip_reg        <= mmio_req_wdata_i[0];
                        TIMER_MTIMECMP_LO: mtimecmp_lo_reg <= mmio_req_wdata_i;
                        TIMER_MTIMECMP_HI: mtimecmp_hi_reg <= mmio_req_wdata_i;
                        TIMER_MTIME_LO:    mtime_lo_reg    <= mmio_req_wdata_i;
                        TIMER_MTIME_HI:    mtime_hi_reg    <= mmio_req_wdata_i;
                        TIMER_CTRL:        timer_en_reg    <= mmio_req_wdata_i[0];
                        default: ;
                    endcase
                    mmio_resp_rdata_o <= 32'h0;
                end else begin
                    unique case (mmio_req_addr_i)
                        TIMER_MSIP:        mmio_resp_rdata_o <= {31'h0, msip_reg};
                        TIMER_MTIMECMP_LO: mmio_resp_rdata_o <= mtimecmp_lo_reg;
                        TIMER_MTIMECMP_HI: mmio_resp_rdata_o <= mtimecmp_hi_reg;
                        TIMER_MTIME_LO:    mmio_resp_rdata_o <= mtime_lo_reg;
                        TIMER_MTIME_HI:    mmio_resp_rdata_o <= mtime_hi_reg;
                        TIMER_CTRL:        mmio_resp_rdata_o <= {31'h0, timer_en_reg};
                        default:           mmio_resp_rdata_o <= 32'h0;
                    endcase
                end
            end
        end
    end

endmodule