//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   AddressGenerateUnit
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   4-level nested-loop address descriptor generator (one of 4
//                AGUs in HDDU).  RTL faithfully follows ESL behavior in
//                Cluster/AddressGenerateUnit.hpp:
//                  * 5-stage internal pipeline s0 -> s1 -> n0_s0 -> n0_s1 -> s2
//                    with s1 -> s2 fast-bypass when intermediates are empty.
//                  * MMIO-mapped configuration / control / status registers.
//                  * 4-level loop counter, idx[0] innermost, idx[3] outermost.
//                  * Tag generation with selectable index source.
// Dependencies:  src/Cluster/cluster_pkg.sv
// Revision:
//   2026/04/27 - Initial version (M1 cluster datapath rewrite)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "cluster_pkg.sv"

module AddressGenerateUnit
    import cluster_pkg::*;
(
    input  logic        clk,
    input  logic        reset_n,

    // MMIO configuration interface (bank-local)
    input  logic        cfg_write,
    input  logic [7:0]  cfg_addr,
    input  logic [31:0] cfg_wdata,
    output logic [31:0] cfg_rdata,

    // External one-shot start / stop pulses (OR'd with CTRL.bit0 / bit1)
    input  logic        start,
    input  logic        stop,

    // Generated descriptor handshake
    output logic        gen_valid,
    input  logic        gen_ready,
    output logic [31:0] gen_addr,
    output logic [15:0] gen_tag,
    output logic        gen_ultra,
    output logic [15:0] gen_mask,

    // Status outputs
    output logic        busy,
    output logic        done,
    output logic [1:0]  fsm_state
);

    // ---------------------------------------------------------------------
    // Configuration registers
    // ---------------------------------------------------------------------
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

    logic        busy_reg;
    logic        done_reg;
    logic        error_reg;
    logic        stalled_reg;

    agu_fsm_e    state_reg;

    // 4-level loop counter
    logic [15:0] idx_reg [0:3];

    // ---------------------------------------------------------------------
    // Pipeline stages
    // ---------------------------------------------------------------------
    typedef struct packed {
        logic        valid;
        logic [15:0] idx0;
        logic [15:0] idx1;
        logic [15:0] idx2;
        logic [15:0] idx3;
        logic [31:0] base_addr;
        logic [31:0] stride0;
        logic [31:0] stride1;
        logic [31:0] stride2;
        logic [31:0] stride3;
        logic [5:0]  tag_base;
        logic [1:0]  tag_level;
        logic [7:0]  tag_stride0;
        logic [7:0]  tag_stride1;
        logic [15:0] mask;
        logic        ultra;
        logic        last;
    } s0_t;

    typedef struct packed {
        logic        valid;
        logic [47:0] prod0;
        logic [47:0] prod1;
        logic [47:0] prod2;
        logic [47:0] prod3;
        logic [31:0] base_addr;
        logic [5:0]  tag_base;
        logic [31:0] tag_mul;
        logic [15:0] mask;
        logic        ultra;
        logic        last;
    } s1_t;

    typedef struct packed {
        logic        valid;
        logic [48:0] s01;
        logic [48:0] s23;
        logic [31:0] base_addr;
        logic [5:0]  tag_base;
        logic [31:0] tag_mul;
        logic [15:0] mask;
        logic        ultra;
        logic        last;
    } n0s0_t;

    typedef struct packed {
        logic        valid;
        logic [49:0] total;
        logic [5:0]  tag_base;
        logic [31:0] tag_mul;
        logic [15:0] mask;
        logic        ultra;
        logic        last;
    } n0s1_t;

    typedef struct packed {
        logic        valid;
        logic [31:0] addr;
        logic [15:0] tag;
        logic [15:0] mask;
        logic        ultra;
        logic        last;
    } s2_t;

    s0_t   s0_reg;
    s1_t   s1_reg;
    n0s0_t n0s0_reg;
    n0s1_t n0s1_reg;
    s2_t   s2_reg;

    logic  run_last_issued_reg;

    // ---------------------------------------------------------------------
    // MMIO read mux (combinational)
    // ---------------------------------------------------------------------
    always_comb begin
        cfg_rdata = 32'h0;
        unique case (cfg_addr)
            AGU_REG_BASE_ADDR:    cfg_rdata = base_addr_reg;
            AGU_REG_BASE_ADDR_H:  cfg_rdata = base_addr_h_reg;
            AGU_REG_ITER01:       cfg_rdata = {iter_reg[1], iter_reg[0]};
            AGU_REG_ITER23:       cfg_rdata = {iter_reg[3], iter_reg[2]};
            AGU_REG_STRIDE0:      cfg_rdata = stride_reg[0];
            AGU_REG_STRIDE1:      cfg_rdata = stride_reg[1];
            AGU_REG_STRIDE2:      cfg_rdata = stride_reg[2];
            AGU_REG_STRIDE3:      cfg_rdata = stride_reg[3];
            AGU_REG_CTRL:         cfg_rdata = ctrl_reg;
            AGU_REG_STATUS: begin
                cfg_rdata = 32'h0;
                cfg_rdata[STATUS_IDLE]     = ~busy_reg & ~done_reg & ~error_reg;
                cfg_rdata[STATUS_BUSY]     = busy_reg;
                cfg_rdata[STATUS_DONE]     = done_reg;
                cfg_rdata[STATUS_QUIESCED] = ~busy_reg & ~stalled_reg;
                cfg_rdata[STATUS_ERROR]    = error_reg;
            end
            AGU_REG_LANE_CFG:     cfg_rdata = lane_cfg_reg;
            AGU_REG_TAG_BASE:     cfg_rdata = tag_base_reg;
            AGU_REG_TAG_STRIDE0:  cfg_rdata = tag_stride0_reg;
            AGU_REG_TAG_STRIDE1:  cfg_rdata = tag_stride1_reg;
            AGU_REG_TAG_CTRL:     cfg_rdata = tag_ctrl_reg;
            AGU_REG_MASK_CFG:     cfg_rdata = mask_cfg_reg;
            AGU_REG_ERR_CODE:     cfg_rdata = err_code_reg;
            AGU_REG_DBG_TAG:      cfg_rdata = dbg_last_tag_reg;
            AGU_REG_DBG_ADDR:     cfg_rdata = dbg_last_addr_reg;
            default:              cfg_rdata = 32'h0;
        endcase
    end

    // ---------------------------------------------------------------------
    // Output drivers (registered)
    // ---------------------------------------------------------------------
    assign gen_valid = s2_reg.valid;
    assign gen_addr  = s2_reg.addr;
    assign gen_tag   = s2_reg.tag;
    assign gen_ultra = s2_reg.ultra;
    assign gen_mask  = s2_reg.mask;
    assign busy      = busy_reg;
    assign done      = done_reg;
    always_comb begin
        unique case (state_reg)
            AGU_FSM_IDLE: fsm_state = 2'd0;
            AGU_FSM_RUN:  fsm_state = 2'd1;
            AGU_FSM_DONE: fsm_state = 2'd2;
            default:      fsm_state = 2'd0;
        endcase
    end

    // ---------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------
    function automatic logic [15:0] norm_iter(input logic [15:0] v);
        norm_iter = (v == 16'd0) ? 16'd1 : v;
    endfunction

    function automatic logic [15:0] calc_tag(input logic [5:0]  tag_base,
                                             input logic [31:0] tag_mul);
        logic [31:0] sum;
        sum = {26'd0, tag_base} + tag_mul;
        calc_tag = {10'd0, sum[5:0]};
    endfunction

    // Compute next loop indices.
    task automatic compute_next_loop(
        input  logic [15:0] cur0, cur1, cur2, cur3,
        output logic [15:0] nxt0, nxt1, nxt2, nxt3,
        output logic        all_done
    );
        logic [16:0] tmp;
        nxt0 = cur0; nxt1 = cur1; nxt2 = cur2; nxt3 = cur3;
        all_done = 1'b0;

        tmp = {1'b0, nxt0} + 17'd1;
        if (tmp[15:0] < iter_reg[0]) begin
            nxt0 = tmp[15:0];
        end else begin
            nxt0 = 16'd0;
            tmp = {1'b0, nxt1} + 17'd1;
            if (tmp[15:0] < iter_reg[1]) begin
                nxt1 = tmp[15:0];
            end else begin
                nxt1 = 16'd0;
                tmp = {1'b0, nxt2} + 17'd1;
                if (tmp[15:0] < iter_reg[2]) begin
                    nxt2 = tmp[15:0];
                end else begin
                    nxt2 = 16'd0;
                    tmp = {1'b0, nxt3} + 17'd1;
                    if (tmp[15:0] < iter_reg[3]) begin
                        nxt3 = tmp[15:0];
                    end else begin
                        nxt3 = 16'd0;
                        all_done = 1'b1;
                    end
                end
            end
        end
    endtask

    // ---------------------------------------------------------------------
    // Main sequential process
    // ---------------------------------------------------------------------
    logic stop_req;
    logic start_req;
    logic soft_reset_req;

    always_comb begin
        stop_req       = stop  | ctrl_reg[CTRL_STOP];
        start_req      = start | ctrl_reg[CTRL_START];
        soft_reset_req = ctrl_reg[CTRL_SOFT_RESET];
    end

    // Pipeline ready signals (combinational, computed from registers each cycle)
    logic s2_ready;
    logic n0s1_ready;
    logic n0s0_ready;
    logic s1_ready;
    logic s0_ready;

    always_comb begin
        s2_ready   = (~s2_reg.valid)   | gen_ready;
        n0s1_ready = (~n0s1_reg.valid) | s2_ready;
        n0s0_ready = (~n0s0_reg.valid) | n0s1_ready;
        s1_ready   = (~s1_reg.valid)   | n0s0_ready;
        s0_ready   = (~s0_reg.valid)   | s1_ready;
    end

    // Helpers used inside seq block.
    s0_t   issue_payload_w;
    logic  issue_payload_valid_w;

    always_comb begin
        issue_payload_w               = '0;
        issue_payload_valid_w         = 1'b0;
        if ((state_reg == AGU_FSM_RUN) && ~run_last_issued_reg && s0_ready) begin
            logic [15:0] nxt0_w, nxt1_w, nxt2_w, nxt3_w;
            logic        all_done_w;

            compute_next_loop(idx_reg[0], idx_reg[1], idx_reg[2], idx_reg[3],
                              nxt0_w, nxt1_w, nxt2_w, nxt3_w, all_done_w);

            issue_payload_valid_w = 1'b1;
            issue_payload_w.valid       = 1'b1;
            issue_payload_w.idx0        = idx_reg[0];
            issue_payload_w.idx1        = idx_reg[1];
            issue_payload_w.idx2        = idx_reg[2];
            issue_payload_w.idx3        = idx_reg[3];
            issue_payload_w.base_addr   = base_addr_reg;
            issue_payload_w.stride0     = stride_reg[0];
            issue_payload_w.stride1     = stride_reg[1];
            issue_payload_w.stride2     = stride_reg[2];
            issue_payload_w.stride3     = stride_reg[3];
            issue_payload_w.tag_base    = tag_base_reg[5:0];
            issue_payload_w.tag_level   = tag_ctrl_reg[1:0];
            issue_payload_w.tag_stride0 = tag_stride0_reg[7:0];
            issue_payload_w.tag_stride1 = tag_stride1_reg[7:0];
            issue_payload_w.mask        = mask_cfg_reg[15:0];
            issue_payload_w.ultra       = ctrl_reg[AGU_CTRL_ULTRA_BIT];
            issue_payload_w.last        = all_done_w;
        end
    end

    // ----- main sequential block -----
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            base_addr_reg     <= 32'h0;
            base_addr_h_reg   <= 32'h0;
            for (int i = 0; i < 4; i++) begin
                iter_reg[i]   <= 16'h1;
                stride_reg[i] <= 32'h0;
                idx_reg[i]    <= 16'h0;
            end
            ctrl_reg          <= 32'h0;
            lane_cfg_reg      <= 32'h0;
            tag_base_reg      <= 32'h0;
            tag_stride0_reg   <= 32'h1;
            tag_stride1_reg   <= 32'h1;
            tag_ctrl_reg      <= 32'h0;
            mask_cfg_reg      <= 32'h0000_000F;
            err_code_reg      <= 32'h0;
            dbg_last_tag_reg  <= 32'h0;
            dbg_last_addr_reg <= 32'h0;

            busy_reg          <= 1'b0;
            done_reg          <= 1'b0;
            error_reg         <= 1'b0;
            stalled_reg       <= 1'b0;

            state_reg         <= AGU_FSM_IDLE;
            run_last_issued_reg <= 1'b0;

            s0_reg            <= '0;
            s1_reg            <= '0;
            n0s0_reg          <= '0;
            n0s1_reg          <= '0;
            s2_reg            <= '0;
        end else begin
            // pulse defaults
            done_reg    <= 1'b0;
            stalled_reg <= 1'b0;

            // ---------------------------------------------------------
            // 1) MMIO write decode
            // ---------------------------------------------------------
            if (cfg_write) begin
                unique case (cfg_addr)
                    AGU_REG_BASE_ADDR:    base_addr_reg   <= cfg_wdata;
                    AGU_REG_BASE_ADDR_H:  base_addr_h_reg <= cfg_wdata;
                    AGU_REG_ITER01: begin
                        iter_reg[0] <= norm_iter(cfg_wdata[15:0]);
                        iter_reg[1] <= norm_iter(cfg_wdata[31:16]);
                    end
                    AGU_REG_ITER23: begin
                        iter_reg[2] <= norm_iter(cfg_wdata[15:0]);
                        iter_reg[3] <= norm_iter(cfg_wdata[31:16]);
                    end
                    AGU_REG_STRIDE0:      stride_reg[0]   <= cfg_wdata;
                    AGU_REG_STRIDE1:      stride_reg[1]   <= cfg_wdata;
                    AGU_REG_STRIDE2:      stride_reg[2]   <= cfg_wdata;
                    AGU_REG_STRIDE3:      stride_reg[3]   <= cfg_wdata;
                    AGU_REG_CTRL:         ctrl_reg        <= cfg_wdata;
                    AGU_REG_LANE_CFG:     lane_cfg_reg    <= cfg_wdata;
                    AGU_REG_TAG_BASE:     tag_base_reg    <= cfg_wdata;
                    AGU_REG_TAG_STRIDE0:  tag_stride0_reg <= cfg_wdata;
                    AGU_REG_TAG_STRIDE1:  tag_stride1_reg <= cfg_wdata;
                    AGU_REG_TAG_CTRL:     tag_ctrl_reg    <= cfg_wdata;
                    AGU_REG_MASK_CFG:     mask_cfg_reg    <= cfg_wdata;
                    AGU_REG_ERR_CODE:     err_code_reg    <= cfg_wdata;
                    default: ;
                endcase
            end

            // ---------------------------------------------------------
            // 2) Soft reset (CTRL.bit2): keep config, clear FSM/pipeline
            // ---------------------------------------------------------
            if (soft_reset_req) begin
                for (int i = 0; i < 4; i++) idx_reg[i] <= 16'h0;
                s0_reg              <= '0;
                s1_reg              <= '0;
                n0s0_reg            <= '0;
                n0s1_reg            <= '0;
                s2_reg              <= '0;
                state_reg           <= AGU_FSM_IDLE;
                busy_reg            <= 1'b0;
                run_last_issued_reg <= 1'b0;
            end

            // ---------------------------------------------------------
            // 3) Stop request
            // ---------------------------------------------------------
            if (stop_req) begin
                state_reg           <= AGU_FSM_IDLE;
                s0_reg              <= '0;
                s1_reg              <= '0;
                n0s0_reg            <= '0;
                n0s1_reg            <= '0;
                s2_reg              <= '0;
                busy_reg            <= 1'b0;
                run_last_issued_reg <= 1'b0;
                if (ctrl_reg[CTRL_STOP]) begin
                    ctrl_reg[CTRL_STOP] <= 1'b0;
                end
            end

            // ---------------------------------------------------------
            // 4) Start request
            // ---------------------------------------------------------
            if (!stop_req && start_req && (state_reg == AGU_FSM_IDLE)) begin
                for (int i = 0; i < 4; i++) idx_reg[i] <= 16'h0;
                s0_reg              <= '0;
                s1_reg              <= '0;
                n0s0_reg            <= '0;
                n0s1_reg            <= '0;
                s2_reg              <= '0;
                state_reg           <= AGU_FSM_RUN;
                busy_reg            <= 1'b1;
                run_last_issued_reg <= 1'b0;
                ctrl_reg[CTRL_START] <= 1'b0;
            end

            if ((state_reg == AGU_FSM_RUN) && ctrl_reg[CTRL_START]) begin
                ctrl_reg[CTRL_START] <= 1'b0;
            end

            // ---------------------------------------------------------
            // 5) RUN pipeline advance
            // ---------------------------------------------------------
            if (!stop_req && !soft_reset_req && (state_reg == AGU_FSM_RUN)) begin
                logic out_fire;
                logic backpressure;
                logic move_n0s1_to_s2;
                logic move_n0s0_to_n0s1;
                logic move_s1_to_n0s0;
                logic move_s0_to_s1;
                logic bypass_s1_to_s2;
                logic move_s1_to_n0s0_eff;
                logic issue_consumed_by_s1;

                out_fire             = s2_reg.valid & gen_ready;
                backpressure         = s2_reg.valid & ~gen_ready;
                move_n0s1_to_s2      = n0s1_reg.valid & s2_ready;
                move_n0s0_to_n0s1    = n0s0_reg.valid & n0s1_ready;
                move_s1_to_n0s0      = s1_reg.valid   & n0s0_ready;
                move_s0_to_s1        = s0_reg.valid   & s1_ready;
                bypass_s1_to_s2      = move_s1_to_n0s0 & ~n0s0_reg.valid & ~n0s1_reg.valid & s2_ready;
                move_s1_to_n0s0_eff  = move_s1_to_n0s0 & ~bypass_s1_to_s2;
                issue_consumed_by_s1 = issue_payload_valid_w & ~move_s0_to_s1 & s1_ready;

                if (out_fire && s2_reg.last) begin
                    state_reg <= AGU_FSM_IDLE;  // ESL: DONE then IDLE next; collapse to single edge
                    busy_reg  <= 1'b0;
                    done_reg  <= 1'b1;
                end else if (out_fire) begin
                    // stay RUN
                end

                if (backpressure) stalled_reg <= 1'b1;

                // ----- s2 update -----
                if (move_n0s1_to_s2) begin
                    s2_reg.valid <= 1'b1;
                    s2_reg.addr  <= n0s1_reg.total[31:0];
                    s2_reg.tag   <= calc_tag(n0s1_reg.tag_base, n0s1_reg.tag_mul);
                    s2_reg.mask  <= n0s1_reg.mask;
                    s2_reg.ultra <= n0s1_reg.ultra;
                    s2_reg.last  <= n0s1_reg.last;
                end else if (bypass_s1_to_s2) begin
                    logic [49:0] s01_w, s23_w, total_w;
                    s01_w   = {2'b0, s1_reg.prod0} + {2'b0, s1_reg.prod1};
                    s23_w   = {2'b0, s1_reg.prod2} + {2'b0, s1_reg.prod3};
                    total_w = s01_w + s23_w + {18'd0, s1_reg.base_addr};
                    s2_reg.valid <= 1'b1;
                    s2_reg.addr  <= total_w[31:0];
                    s2_reg.tag   <= calc_tag(s1_reg.tag_base, s1_reg.tag_mul);
                    s2_reg.mask  <= s1_reg.mask;
                    s2_reg.ultra <= s1_reg.ultra;
                    s2_reg.last  <= s1_reg.last;
                end else if (s2_ready) begin
                    s2_reg.valid <= 1'b0;
                end

                // ----- n0s1 update -----
                if (move_n0s0_to_n0s1) begin
                    logic [49:0] total_w;
                    total_w = {1'b0, n0s0_reg.s01} + {1'b0, n0s0_reg.s23} + {18'd0, n0s0_reg.base_addr};
                    n0s1_reg.valid    <= 1'b1;
                    n0s1_reg.total    <= total_w;
                    n0s1_reg.tag_base <= n0s0_reg.tag_base;
                    n0s1_reg.tag_mul  <= n0s0_reg.tag_mul;
                    n0s1_reg.mask     <= n0s0_reg.mask;
                    n0s1_reg.ultra    <= n0s0_reg.ultra;
                    n0s1_reg.last     <= n0s0_reg.last;
                end else if (n0s1_ready) begin
                    n0s1_reg.valid <= 1'b0;
                end

                // ----- n0s0 update -----
                if (move_s1_to_n0s0_eff) begin
                    logic [48:0] s01_w, s23_w;
                    s01_w = {1'b0, s1_reg.prod0} + {1'b0, s1_reg.prod1};
                    s23_w = {1'b0, s1_reg.prod2} + {1'b0, s1_reg.prod3};
                    n0s0_reg.valid     <= 1'b1;
                    n0s0_reg.s01       <= s01_w;
                    n0s0_reg.s23       <= s23_w;
                    n0s0_reg.base_addr <= s1_reg.base_addr;
                    n0s0_reg.tag_base  <= s1_reg.tag_base;
                    n0s0_reg.tag_mul   <= s1_reg.tag_mul;
                    n0s0_reg.mask      <= s1_reg.mask;
                    n0s0_reg.ultra     <= s1_reg.ultra;
                    n0s0_reg.last      <= s1_reg.last;
                end else if (n0s0_ready) begin
                    n0s0_reg.valid <= 1'b0;
                end

                // ----- s1 update -----
                if (s1_ready) begin
                    s0_t src_w;
                    logic src_valid;
                    logic [31:0] tag_index_w;
                    logic [31:0] tag_stride_w;
                    logic [47:0] prod0_w;
                    logic [47:0] prod1_w;
                    logic [47:0] prod2_w;
                    logic [47:0] prod3_w;

                    src_w     = '0;
                    src_valid = 1'b0;
                    prod0_w   = '0;
                    prod1_w   = '0;
                    prod2_w   = '0;
                    prod3_w   = '0;

                    if (move_s0_to_s1) begin
                        src_w     = s0_reg;
                        src_valid = 1'b1;
                    end else if (issue_payload_valid_w) begin
                        src_w     = issue_payload_w;
                        src_valid = 1'b1;
                    end

                    if (src_valid) begin
                        unique case (src_w.tag_level)
                            2'd0: begin
                                tag_index_w  = {16'd0, src_w.idx0};
                                tag_stride_w = {24'd0, src_w.tag_stride0};
                            end
                            2'd1: begin
                                tag_index_w  = {16'd0, src_w.idx1};
                                tag_stride_w = {24'd0, src_w.tag_stride1};
                            end
                            2'd2: begin
                                tag_index_w  = {16'd0, src_w.idx2};
                                tag_stride_w = {24'd0, src_w.tag_stride1};
                            end
                            default: begin
                                tag_index_w  = {16'd0, src_w.idx3};
                                tag_stride_w = {24'd0, src_w.tag_stride1};
                            end
                        endcase

                        prod0_w = {32'd0, src_w.idx0} * {16'd0, src_w.stride0};
                        prod1_w = {32'd0, src_w.idx1} * {16'd0, src_w.stride1};
                        prod2_w = {32'd0, src_w.idx2} * {16'd0, src_w.stride2};
                        prod3_w = {32'd0, src_w.idx3} * {16'd0, src_w.stride3};

                        s1_reg.valid     <= 1'b1;
                        s1_reg.base_addr <= src_w.base_addr;
                        s1_reg.prod0     <= prod0_w;
                        s1_reg.prod1     <= prod1_w;
                        s1_reg.prod2     <= prod2_w;
                        s1_reg.prod3     <= prod3_w;
                        s1_reg.tag_base  <= src_w.tag_base;
                        s1_reg.tag_mul   <= tag_index_w * tag_stride_w;
                        s1_reg.mask      <= src_w.mask;
                        s1_reg.ultra     <= src_w.ultra;
                        s1_reg.last      <= src_w.last;
                    end else begin
                        s1_reg.valid <= 1'b0;
                    end
                end

                // ----- s0 update -----
                if (s0_ready) begin
                    if (issue_payload_valid_w && !issue_consumed_by_s1) begin
                        s0_reg <= issue_payload_w;
                    end else begin
                        s0_reg.valid <= 1'b0;
                    end
                end

                // ----- loop counter advance + last-issued tracking -----
                if (issue_payload_valid_w &&
                    (issue_consumed_by_s1 || (s0_ready && !issue_consumed_by_s1))) begin
                    logic [15:0] nxt0_w, nxt1_w, nxt2_w, nxt3_w;
                    logic        all_done_w;
                    compute_next_loop(idx_reg[0], idx_reg[1], idx_reg[2], idx_reg[3],
                                      nxt0_w, nxt1_w, nxt2_w, nxt3_w, all_done_w);
                    idx_reg[0] <= nxt0_w;
                    idx_reg[1] <= nxt1_w;
                    idx_reg[2] <= nxt2_w;
                    idx_reg[3] <= nxt3_w;
                    if (all_done_w) run_last_issued_reg <= 1'b1;
                end

                // ----- debug capture -----
                if (s2_reg.valid) begin
                    dbg_last_addr_reg <= s2_reg.addr;
                    dbg_last_tag_reg  <= {16'd0, s2_reg.tag};
                end
            end else if (state_reg != AGU_FSM_RUN) begin
                // Outside RUN: clear pipeline + outputs
                s0_reg   <= '0;
                s1_reg   <= '0;
                n0s0_reg <= '0;
                n0s1_reg <= '0;
                s2_reg   <= '0;
            end
        end
    end

endmodule
