// Module: AddressGenerateUnit
// Function: MMIO-configurable nested-loop address generator that emits address, tag, ultra, and mask descriptors.
module AddressGenerateUnit (
    input  logic        clk,
    input  logic        reset_n,
    input  logic        cfg_write,
    input  logic [7:0]  cfg_addr,
    input  logic [31:0] cfg_wdata,
    output logic [31:0] cfg_rdata,
    input  logic        start,
    input  logic        stop,
    output logic        gen_valid,
    input  logic        gen_ready,
    output logic [31:0] gen_addr,
    output logic [15:0] gen_tag,
    output logic        gen_ultra,
    output logic [15:0] gen_mask,
    output logic        busy,
    output logic        done,
    output logic [1:0]  fsm_state
);
    localparam logic [7:0] REG_BASE_ADDR   = 8'h00;
    localparam logic [7:0] REG_BASE_ADDR_H = 8'h04;
    localparam logic [7:0] REG_ITER01      = 8'h08;
    localparam logic [7:0] REG_ITER23      = 8'h0C;
    localparam logic [7:0] REG_STRIDE0     = 8'h10;
    localparam logic [7:0] REG_STRIDE1     = 8'h14;
    localparam logic [7:0] REG_STRIDE2     = 8'h18;
    localparam logic [7:0] REG_STRIDE3     = 8'h1C;
    localparam logic [7:0] REG_CTRL        = 8'h20;
    localparam logic [7:0] REG_STATUS      = 8'h24;
    localparam logic [7:0] REG_LANE_CFG    = 8'h28;
    localparam logic [7:0] REG_TAG_BASE    = 8'h40;
    localparam logic [7:0] REG_TAG_STRIDE0 = 8'h44;
    localparam logic [7:0] REG_TAG_STRIDE1 = 8'h48;
    localparam logic [7:0] REG_TAG_CTRL    = 8'h4C;
    localparam logic [7:0] REG_MASK_CFG    = 8'h54;
    localparam logic [7:0] REG_ERR_CODE    = 8'h58;
    localparam logic [7:0] REG_DBG_TAG     = 8'h5C;
    localparam logic [7:0] REG_DBG_ADDR    = 8'h60;

    typedef enum logic [1:0] {
        AGU_IDLE = 2'd0,
        AGU_RUN  = 2'd1,
        AGU_DONE = 2'd2
    } agu_state_t;

    agu_state_t state_reg;

    logic [31:0] base_addr_reg;
    logic [31:0] base_addr_h_reg;
    logic [15:0] iter_reg [0:3];
    logic [31:0] stride_reg [0:3];
    logic [31:0] ctrl_reg;
    logic [31:0] lane_cfg_reg;
    logic [31:0] tag_base_reg;
    logic [31:0] tag_stride0_reg;
    logic [31:0] tag_stride1_reg;
    logic [31:0] tag_ctrl_reg;
    logic [31:0] mask_cfg_reg;
    logic [31:0] err_code_reg;
    logic [31:0] dbg_last_tag_reg;
    logic [31:0] dbg_last_addr_reg;
    logic        error_reg;
    logic        stalled_reg;

    logic [15:0] idx_reg [0:3];

    logic        out_valid_reg;
    logic [31:0] out_addr_reg;
    logic [15:0] out_tag_reg;
    logic        out_ultra_reg;
    logic [15:0] out_mask_reg;
    logic        out_last_reg;

    function automatic logic [15:0] normalize_iter(input logic [15:0] value);
        normalize_iter = (value == 16'h0000) ? 16'h0001 : value;
    endfunction

    function automatic logic is_last_index(
        input logic [15:0] idx0,
        input logic [15:0] idx1,
        input logic [15:0] idx2,
        input logic [15:0] idx3,
        input logic [15:0] iter0,
        input logic [15:0] iter1,
        input logic [15:0] iter2,
        input logic [15:0] iter3
    );
        is_last_index = (idx0 == (iter0 - 16'd1))
                     && (idx1 == (iter1 - 16'd1))
                     && (idx2 == (iter2 - 16'd1))
                     && (idx3 == (iter3 - 16'd1));
    endfunction

    function automatic logic [31:0] calc_addr(
        input logic [31:0] base_addr,
        input logic [15:0] idx0,
        input logic [15:0] idx1,
        input logic [15:0] idx2,
        input logic [15:0] idx3,
        input logic [31:0] stride0,
        input logic [31:0] stride1,
        input logic [31:0] stride2,
        input logic [31:0] stride3
    );
        logic [63:0] total;
        total = base_addr;
        total += idx0 * stride0;
        total += idx1 * stride1;
        total += idx2 * stride2;
        total += idx3 * stride3;
        calc_addr = total[31:0];
    endfunction

    function automatic logic [15:0] calc_tag(
        input logic [5:0]  tag_base,
        input logic [1:0]  tag_level,
        input logic [7:0]  tag_stride0,
        input logic [7:0]  tag_stride1,
        input logic [15:0] idx0,
        input logic [15:0] idx1,
        input logic [15:0] idx2,
        input logic [15:0] idx3
    );
        logic [31:0] tag_index;
        logic [31:0] tag_stride;
        logic [31:0] tag_total;
        begin
            unique case (tag_level)
                2'd0: begin
                    tag_index = idx0;
                    tag_stride = tag_stride0;
                end
                2'd1: begin
                    tag_index = idx1;
                    tag_stride = tag_stride1;
                end
                2'd2: begin
                    tag_index = idx2;
                    tag_stride = tag_stride1;
                end
                default: begin
                    tag_index = idx3;
                    tag_stride = tag_stride1;
                end
            endcase
            tag_total = tag_base + (tag_index * tag_stride);
            calc_tag = tag_total[5:0];
        end
    endfunction

    function automatic logic [1:0] state_to_bits(input agu_state_t state);
        unique case (state)
            AGU_IDLE: state_to_bits = 2'd0;
            AGU_RUN:  state_to_bits = 2'd1;
            AGU_DONE: state_to_bits = 2'd2;
            default:  state_to_bits = 2'd0;
        endcase
    endfunction

    always_comb begin
        unique case (cfg_addr)
            REG_BASE_ADDR:   cfg_rdata = base_addr_reg;
            REG_BASE_ADDR_H: cfg_rdata = base_addr_h_reg;
            REG_ITER01:      cfg_rdata = {iter_reg[1], iter_reg[0]};
            REG_ITER23:      cfg_rdata = {iter_reg[3], iter_reg[2]};
            REG_STRIDE0:     cfg_rdata = stride_reg[0];
            REG_STRIDE1:     cfg_rdata = stride_reg[1];
            REG_STRIDE2:     cfg_rdata = stride_reg[2];
            REG_STRIDE3:     cfg_rdata = stride_reg[3];
            REG_CTRL:        cfg_rdata = ctrl_reg;
            REG_STATUS:      cfg_rdata = {28'h0, stalled_reg, error_reg, done, busy};
            REG_LANE_CFG:    cfg_rdata = lane_cfg_reg;
            REG_TAG_BASE:    cfg_rdata = tag_base_reg;
            REG_TAG_STRIDE0: cfg_rdata = tag_stride0_reg;
            REG_TAG_STRIDE1: cfg_rdata = tag_stride1_reg;
            REG_TAG_CTRL:    cfg_rdata = tag_ctrl_reg;
            REG_MASK_CFG:    cfg_rdata = mask_cfg_reg;
            REG_ERR_CODE:    cfg_rdata = err_code_reg;
            REG_DBG_TAG:     cfg_rdata = dbg_last_tag_reg;
            REG_DBG_ADDR:    cfg_rdata = dbg_last_addr_reg;
            default:         cfg_rdata = 32'h0;
        endcase
    end

    assign gen_valid = out_valid_reg;
    assign gen_addr  = out_addr_reg;
    assign gen_tag   = out_tag_reg;
    assign gen_ultra = out_ultra_reg;
    assign gen_mask  = out_mask_reg;
    assign fsm_state = state_to_bits(state_reg);

    always_ff @(posedge clk or negedge reset_n) begin
        logic start_req;
        logic stop_req;
        logic soft_reset_req;
        logic [31:0] next_addr;
        logic [15:0] next_tag;
        logic next_last;
        logic [15:0] next_idx0;
        logic [15:0] next_idx1;
        logic [15:0] next_idx2;
        logic [15:0] next_idx3;

        if (!reset_n) begin
            base_addr_reg   <= 32'h0;
            base_addr_h_reg <= 32'h0;
            iter_reg[0]     <= 16'd1;
            iter_reg[1]     <= 16'd1;
            iter_reg[2]     <= 16'd1;
            iter_reg[3]     <= 16'd1;
            stride_reg[0]   <= 32'h0;
            stride_reg[1]   <= 32'h0;
            stride_reg[2]   <= 32'h0;
            stride_reg[3]   <= 32'h0;
            ctrl_reg        <= 32'h0;
            lane_cfg_reg    <= 32'h0;
            tag_base_reg    <= 32'h0;
            tag_stride0_reg <= 32'h1;
            tag_stride1_reg <= 32'h1;
            tag_ctrl_reg    <= 32'h0;
            mask_cfg_reg    <= 32'h0000_000F;
            err_code_reg    <= 32'h0;
            dbg_last_tag_reg <= 32'h0;
            dbg_last_addr_reg <= 32'h0;
            error_reg       <= 1'b0;
            stalled_reg     <= 1'b0;
            idx_reg[0]      <= 16'h0;
            idx_reg[1]      <= 16'h0;
            idx_reg[2]      <= 16'h0;
            idx_reg[3]      <= 16'h0;
            out_valid_reg   <= 1'b0;
            out_addr_reg    <= 32'h0;
            out_tag_reg     <= 16'h0;
            out_ultra_reg   <= 1'b0;
            out_mask_reg    <= 16'h0;
            out_last_reg    <= 1'b0;
            busy            <= 1'b0;
            done            <= 1'b0;
            state_reg       <= AGU_IDLE;
        end else begin
            done <= 1'b0;
            stalled_reg <= 1'b0;

            if (cfg_write) begin
                unique case (cfg_addr)
                    REG_BASE_ADDR:   base_addr_reg <= cfg_wdata;
                    REG_BASE_ADDR_H: base_addr_h_reg <= cfg_wdata;
                    REG_ITER01: begin
                        iter_reg[0] <= normalize_iter(cfg_wdata[15:0]);
                        iter_reg[1] <= normalize_iter(cfg_wdata[31:16]);
                    end
                    REG_ITER23: begin
                        iter_reg[2] <= normalize_iter(cfg_wdata[15:0]);
                        iter_reg[3] <= normalize_iter(cfg_wdata[31:16]);
                    end
                    REG_STRIDE0: stride_reg[0] <= cfg_wdata;
                    REG_STRIDE1: stride_reg[1] <= cfg_wdata;
                    REG_STRIDE2: stride_reg[2] <= cfg_wdata;
                    REG_STRIDE3: stride_reg[3] <= cfg_wdata;
                    REG_CTRL:    ctrl_reg <= cfg_wdata;
                    REG_LANE_CFG: lane_cfg_reg <= cfg_wdata;
                    REG_TAG_BASE: tag_base_reg <= cfg_wdata;
                    REG_TAG_STRIDE0: tag_stride0_reg <= cfg_wdata;
                    REG_TAG_STRIDE1: tag_stride1_reg <= cfg_wdata;
                    REG_TAG_CTRL: tag_ctrl_reg <= cfg_wdata;
                    REG_MASK_CFG: mask_cfg_reg <= cfg_wdata;
                    REG_ERR_CODE: err_code_reg <= cfg_wdata;
                    default: ;
                endcase
            end

            start_req = start | ctrl_reg[0];
            stop_req = stop | ctrl_reg[1];
            soft_reset_req = ctrl_reg[2];

            if (soft_reset_req) begin
                idx_reg[0] <= 16'h0;
                idx_reg[1] <= 16'h0;
                idx_reg[2] <= 16'h0;
                idx_reg[3] <= 16'h0;
                out_valid_reg <= 1'b0;
                out_last_reg <= 1'b0;
                busy <= 1'b0;
                state_reg <= AGU_IDLE;
                ctrl_reg[2] <= 1'b0;
            end else if (stop_req) begin
                idx_reg[0] <= 16'h0;
                idx_reg[1] <= 16'h0;
                idx_reg[2] <= 16'h0;
                idx_reg[3] <= 16'h0;
                out_valid_reg <= 1'b0;
                out_last_reg <= 1'b0;
                busy <= 1'b0;
                state_reg <= AGU_IDLE;
                ctrl_reg[1] <= 1'b0;
            end else begin
                unique case (state_reg)
                    AGU_IDLE: begin
                        out_valid_reg <= 1'b0;
                        out_last_reg <= 1'b0;
                        busy <= 1'b0;
                        if (start_req) begin
                            idx_reg[0] <= 16'h0;
                            idx_reg[1] <= 16'h0;
                            idx_reg[2] <= 16'h0;
                            idx_reg[3] <= 16'h0;
                            state_reg <= AGU_RUN;
                            busy <= 1'b1;
                            ctrl_reg[0] <= 1'b0;
                        end
                    end
                    AGU_RUN: begin
                        busy <= 1'b1;

                        if (!out_valid_reg) begin
                            next_addr = calc_addr(
                                base_addr_reg,
                                idx_reg[0], idx_reg[1], idx_reg[2], idx_reg[3],
                                stride_reg[0], stride_reg[1], stride_reg[2], stride_reg[3]
                            );
                            next_tag = calc_tag(
                                tag_base_reg[5:0],
                                tag_ctrl_reg[1:0],
                                tag_stride0_reg[7:0],
                                tag_stride1_reg[7:0],
                                idx_reg[0], idx_reg[1], idx_reg[2], idx_reg[3]
                            );
                            next_last = is_last_index(
                                idx_reg[0], idx_reg[1], idx_reg[2], idx_reg[3],
                                iter_reg[0], iter_reg[1], iter_reg[2], iter_reg[3]
                            );
                            out_valid_reg <= 1'b1;
                            out_addr_reg <= next_addr;
                            out_tag_reg <= next_tag;
                            out_ultra_reg <= ctrl_reg[3];
                            out_mask_reg <= mask_cfg_reg[15:0];
                            out_last_reg <= next_last;
                            dbg_last_addr_reg <= next_addr;
                            dbg_last_tag_reg <= {26'h0, next_tag[5:0]};
                        end else if (gen_ready) begin
                            if (out_last_reg) begin
                                out_valid_reg <= 1'b0;
                                out_last_reg <= 1'b0;
                                busy <= 1'b0;
                                done <= 1'b1;
                                state_reg <= AGU_DONE;
                            end else begin
                                next_idx0 = idx_reg[0] + 16'd1;
                                next_idx1 = idx_reg[1];
                                next_idx2 = idx_reg[2];
                                next_idx3 = idx_reg[3];

                                if (next_idx0 >= iter_reg[0]) begin
                                    next_idx0 = 16'h0;
                                    next_idx1 = idx_reg[1] + 16'd1;
                                    if (next_idx1 >= iter_reg[1]) begin
                                        next_idx1 = 16'h0;
                                        next_idx2 = idx_reg[2] + 16'd1;
                                        if (next_idx2 >= iter_reg[2]) begin
                                            next_idx2 = 16'h0;
                                            next_idx3 = idx_reg[3] + 16'd1;
                                            if (next_idx3 >= iter_reg[3]) begin
                                                next_idx3 = 16'h0;
                                            end
                                        end
                                    end
                                end

                                idx_reg[0] <= next_idx0;
                                idx_reg[1] <= next_idx1;
                                idx_reg[2] <= next_idx2;
                                idx_reg[3] <= next_idx3;
                                out_valid_reg <= 1'b0;
                            end
                        end else begin
                            stalled_reg <= 1'b1;
                        end
                    end
                    AGU_DONE: begin
                        state_reg <= AGU_IDLE;
                        busy <= 1'b0;
                        out_valid_reg <= 1'b0;
                    end
                    default: begin
                        state_reg <= AGU_IDLE;
                        busy <= 1'b0;
                        out_valid_reg <= 1'b0;
                    end
                endcase
            end
        end
    end
endmodule