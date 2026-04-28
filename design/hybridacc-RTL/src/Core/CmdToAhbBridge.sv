//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   CmdToAhbBridge
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Simple native-command to AHB-Lite bridge baseline.
// Dependencies:  None
// Revision:
//   2026/04/27 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
module CmdToAhbBridge (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        cmd_req_valid_i,
    input  logic        cmd_req_write_i,
    input  logic [31:0] cmd_req_addr_i,
    input  logic [31:0] cmd_req_wdata_i,
    input  logic [3:0]  cmd_req_wstrb_i,
    output logic        cmd_req_ready_o,
    output logic        cmd_resp_valid_o,
    output logic [31:0] cmd_resp_rdata_o,
    output logic        hsel_o,
    output logic [31:0] haddr_o,
    output logic        hwrite_o,
    output logic [1:0]  htrans_o,
    output logic [2:0]  hsize_o,
    output logic [2:0]  hburst_o,
    output logic [3:0]  hprot_o,
    output logic [31:0] hwdata_o,
    input  logic        hready_i,
    input  logic        hresp_i,
    input  logic [31:0] hrdata_i
);
    typedef enum logic [1:0] {
        BR_IDLE,
        BR_ADDR,
        BR_RESP
    } bridge_state_e;

    bridge_state_e state_reg;
    logic        latched_write_reg;
    logic [31:0] latched_addr_reg;
    logic [31:0] latched_wdata_reg;

    assign cmd_req_ready_o = (state_reg == BR_IDLE);
    assign hsize_o = 3'b010;
    assign hburst_o = 3'b000;
    assign hprot_o = {3'b001, |cmd_req_wstrb_i};
    assign cmd_resp_rdata_o = hrdata_i;

    always_comb begin
        hsel_o = 1'b0;
        haddr_o = latched_addr_reg;
        hwrite_o = latched_write_reg;
        htrans_o = 2'b00;
        hwdata_o = latched_wdata_reg;
        cmd_resp_valid_o = 1'b0;

        unique case (state_reg)
            BR_ADDR: begin
                hsel_o = 1'b1;
                htrans_o = 2'b10;
            end
            BR_RESP: begin
                cmd_resp_valid_o = hready_i;
            end
            default: ;
        endcase
    end

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state_reg <= BR_IDLE;
            latched_write_reg <= 1'b0;
            latched_addr_reg <= 32'h0;
            latched_wdata_reg <= 32'h0;
        end else begin
            unique case (state_reg)
                BR_IDLE: begin
                    if (cmd_req_valid_i) begin
                        latched_write_reg <= cmd_req_write_i;
                        latched_addr_reg <= cmd_req_addr_i;
                        latched_wdata_reg <= cmd_req_wdata_i;
                        state_reg <= BR_ADDR;
                    end
                end
                BR_ADDR: begin
                    if (hready_i) begin
                        state_reg <= BR_RESP;
                    end
                end
                BR_RESP: begin
                    if (hready_i) begin
                        state_reg <= BR_IDLE;
                    end
                end
                default: state_reg <= BR_IDLE;
            endcase
        end
    end

endmodule