//-----------------------------------------------------------------------------
// Module Name:   HybridDataDeliverUnit
// Description:   RTL conversion of ESL HybridDataDeliverUnit.hpp
//                4 AGUs, 3 send planes (PS/PD/PLI -> SPM read -> NoC) +
//                1 receive plane (PLO: NoC -> SPM write), MMIO, counters.
//-----------------------------------------------------------------------------
module HybridDataDeliverUnit
    import hybridacc_utils_pkg::*;
#(
    parameter int unsigned SPM_ADDR_BITS  = 32,
    parameter int unsigned NOC_TAG_BITS   = 6,
    parameter int unsigned DATA_BITS      = 192
) (
    input  logic        clk,
    input  logic        reset_n,

    output logic [3:0]                     spm_req_valid,
    input  logic [3:0]                     spm_req_ready,
    output logic [3:0][SPM_ADDR_BITS-1:0]  spm_req_addr,
    output logic [3:0][DATA_BITS-1:0]      spm_req_wdata,
    output logic [3:0]                     spm_req_wen,

    input  logic [3:0]                     spm_resp_valid,
    output logic [3:0]                     spm_resp_ready,
    input  logic [3:0][DATA_BITS-1:0]      spm_resp_rdata,
    input  SPM_RESPONSE_CODE               spm_resp_code [4],

    output logic                           noc_ps_valid,
    input  logic                           noc_ps_ready,
    output logic [DATA_BITS-1:0]           noc_ps_data,
    output logic [15:0]                    noc_ps_addr,
    output logic [DATA_BITS-1:0]           noc_ps_mask,

    output logic                           noc_pd_valid,
    input  logic                           noc_pd_ready,
    output logic [DATA_BITS-1:0]           noc_pd_data,
    output logic [15:0]                    noc_pd_addr,
    output logic [DATA_BITS-1:0]           noc_pd_mask,

    output logic                           noc_pli_valid,
    input  logic                           noc_pli_ready,
    output logic [DATA_BITS-1:0]           noc_pli_data,
    output logic [15:0]                    noc_pli_addr,
    output logic [DATA_BITS-1:0]           noc_pli_mask,

    output logic                           noc_plo_req_valid,
    input  logic                           noc_plo_req_ready,
    output logic [15:0]                    noc_plo_req_addr,

    input  logic                           noc_plo_resp_valid,
    output logic                           noc_plo_resp_ready,
    input  logic [DATA_BITS-1:0]           noc_plo_resp_data,

    input  logic [31:0]                    mmio_addr,
    input  logic                           mmio_write,
    input  logic [31:0]                    mmio_wdata,
    output logic [31:0]                    mmio_rdata,

    output logic                           interrupt
);

    localparam int unsigned NUM_AGU         = 4;
    localparam int unsigned NUM_SPM         = 4;
    localparam int unsigned NUM_SEND_PLANES = 3;
    localparam int unsigned RECV_PLANE      = 3;
    localparam int unsigned NOC_ADDR_BITS   = NOC_TAG_BITS + 1;
    localparam int unsigned DATA_BYTES      = DATA_BITS / 8;
    localparam int unsigned FIFO_DEPTH      = 16;

    localparam logic [31:0] MMIO_AGU_BASE       = 32'h000;
    localparam logic [31:0] MMIO_AGU_BANK_STRIDE= 32'h100;
    localparam logic [31:0] MMIO_AGU_SIZE       = NUM_AGU * 32'h100;
    localparam logic [31:0] MMIO_AGU_BANK_MASK  = 32'h3;
    localparam logic [31:0] MMIO_AGU_SUB_MASK   = 32'hFF;
    localparam logic [7:0]  MMIO_AGU_CTRL_OFFSET= 8'h20;

    localparam logic [31:0] MMIO_GLOBAL_BASE        = 32'h800;
    localparam logic [31:0] MMIO_GLOBAL_END         = 32'h8FF;
    localparam logic [31:0] MMIO_GLOBAL_CTRL        = 32'h800;
    localparam logic [31:0] MMIO_GLOBAL_STATUS      = 32'h804;
    localparam logic [31:0] MMIO_GLOBAL_PLANE_EN    = 32'h808;
    localparam logic [31:0] MMIO_GLOBAL_PLANE_MODE  = 32'h80C;
    localparam logic [31:0] MMIO_GLOBAL_NUM_PLANES  = 32'h810;
    localparam logic [31:0] MMIO_GLOBAL_PORT_WIDTH  = 32'h814;
    localparam logic [31:0] MMIO_GLOBAL_ARB_POLICY  = 32'h818;
    localparam logic [31:0] MMIO_GLOBAL_ERR_CODE    = 32'h81C;
    localparam logic [31:0] MMIO_GLOBAL_ERR_INFO0   = 32'h820;
    localparam logic [31:0] MMIO_GLOBAL_ERR_INFO1   = 32'h824;
    localparam logic [31:0] MMIO_GLOBAL_CNT_TX_PKT  = 32'h828;
    localparam logic [31:0] MMIO_GLOBAL_CNT_TX_BYTE = 32'h82C;
    localparam logic [31:0] MMIO_GLOBAL_CNT_RX_BYTE = 32'h830;
    localparam logic [31:0] MMIO_GLOBAL_CNT_STALL   = 32'h834;

    localparam logic [31:0] DEFAULT_PLANE_EN = (1 << NUM_AGU) - 1;

    typedef struct packed {
        logic [DATA_BITS-1:0]  data;
        logic [15:0]           addr;
        logic [DATA_BITS-1:0]  mask;
    } noc_req_payload_t;

    typedef struct packed {
        logic [SPM_ADDR_BITS-1:0] addr;
        logic [DATA_BITS-1:0]     wdata;
        logic                     wen;
    } spm_req_payload_t;

    // =========================================================================
    // AGU signals and instances
    // =========================================================================
    logic        agu_cfg_write  [NUM_AGU];
    logic [7:0]  agu_cfg_addr   [NUM_AGU];
    logic [31:0] agu_cfg_wdata  [NUM_AGU];
    logic [31:0] agu_cfg_rdata  [NUM_AGU];
    logic        agu_start      [NUM_AGU];
    logic        agu_stop       [NUM_AGU];
    logic        agu_gen_valid  [NUM_AGU];
    logic        agu_gen_ready  [NUM_AGU];
    logic [31:0] agu_gen_addr   [NUM_AGU];
    logic [15:0] agu_gen_tag    [NUM_AGU];
    logic        agu_gen_ultra  [NUM_AGU];
    logic [15:0] agu_gen_mask   [NUM_AGU];
    logic        agu_busy       [NUM_AGU];
    logic        agu_done       [NUM_AGU];
    logic [1:0]  agu_fsm_state  [NUM_AGU];

    genvar gv;
    generate
        for (gv = 0; gv < NUM_AGU; gv++) begin : gen_agu
            AddressGenerateUnit u_agu (
                .clk(clk), .reset_n(reset_n),
                .cfg_write(agu_cfg_write[gv]), .cfg_addr(agu_cfg_addr[gv]),
                .cfg_wdata(agu_cfg_wdata[gv]), .cfg_rdata(agu_cfg_rdata[gv]),
                .start(agu_start[gv]), .stop(agu_stop[gv]),
                .gen_valid(agu_gen_valid[gv]), .gen_ready(agu_gen_ready[gv]),
                .gen_addr(agu_gen_addr[gv]), .gen_tag(agu_gen_tag[gv]),
                .gen_ultra(agu_gen_ultra[gv]), .gen_mask(agu_gen_mask[gv]),
                .busy(agu_busy[gv]), .done(agu_done[gv]), .fsm_state(agu_fsm_state[gv])
            );
        end
    endgenerate

    // =========================================================================
    // Internal FIFOs
    // =========================================================================
    logic               noc_req_fifo_empty [NUM_SEND_PLANES];
    logic               noc_req_fifo_full  [NUM_SEND_PLANES];
    noc_req_payload_t   noc_req_fifo_din   [NUM_SEND_PLANES];
    noc_req_payload_t   noc_req_fifo_dout  [NUM_SEND_PLANES];
    logic               noc_req_fifo_push  [NUM_SEND_PLANES];
    logic               noc_req_fifo_pop   [NUM_SEND_PLANES];
    logic               noc_req_fifo_clear [NUM_SEND_PLANES];

    generate
        for (gv = 0; gv < NUM_SEND_PLANES; gv++) begin : gen_noc_req_fifo
            FIFO #(.T(noc_req_payload_t), .DEPTH(FIFO_DEPTH)) u_noc_req_fifo (
                .clk(clk), .reset_n(reset_n),
                .data_in(noc_req_fifo_din[gv]), .push(noc_req_fifo_push[gv]),
                .data_out(noc_req_fifo_dout[gv]), .pop(noc_req_fifo_pop[gv]),
                .empty(noc_req_fifo_empty[gv]), .full(noc_req_fifo_full[gv]),
                .clear(noc_req_fifo_clear[gv])
            );
        end
    endgenerate

    logic                       wait_fifo_empty [NUM_SEND_PLANES];
    logic                       wait_fifo_full  [NUM_SEND_PLANES];
    logic [NOC_ADDR_BITS-1:0]   wait_fifo_din   [NUM_SEND_PLANES];
    logic [NOC_ADDR_BITS-1:0]   wait_fifo_dout  [NUM_SEND_PLANES];
    logic                       wait_fifo_push  [NUM_SEND_PLANES];
    logic                       wait_fifo_pop   [NUM_SEND_PLANES];
    logic                       wait_fifo_clear [NUM_SEND_PLANES];

    generate
        for (gv = 0; gv < NUM_SEND_PLANES; gv++) begin : gen_wait_fifo
            FIFO #(.T(logic [NOC_ADDR_BITS-1:0]), .DEPTH(FIFO_DEPTH)) u_wait_fifo (
                .clk(clk), .reset_n(reset_n),
                .data_in(wait_fifo_din[gv]), .push(wait_fifo_push[gv]),
                .data_out(wait_fifo_dout[gv]), .pop(wait_fifo_pop[gv]),
                .empty(wait_fifo_empty[gv]), .full(wait_fifo_full[gv]),
                .clear(wait_fifo_clear[gv])
            );
        end
    endgenerate

    logic                       write_addr_fifo_empty, write_addr_fifo_full;
    logic [SPM_ADDR_BITS-1:0]   write_addr_fifo_din, write_addr_fifo_dout;
    logic                       write_addr_fifo_push, write_addr_fifo_pop, write_addr_fifo_clear;

    FIFO #(.T(logic [SPM_ADDR_BITS-1:0]), .DEPTH(FIFO_DEPTH)) u_write_addr_fifo (
        .clk(clk), .reset_n(reset_n),
        .data_in(agu_gen_addr[RECV_PLANE][SPM_ADDR_BITS-1:0]),
        .push(write_addr_fifo_push),
        .data_out(write_addr_fifo_dout), .pop(write_addr_fifo_pop),
        .empty(write_addr_fifo_empty), .full(write_addr_fifo_full),
        .clear(write_addr_fifo_clear)
    );

    logic               spm_wr_fifo_empty, spm_wr_fifo_full;
    spm_req_payload_t   spm_wr_fifo_din, spm_wr_fifo_dout;
    logic               spm_wr_fifo_push, spm_wr_fifo_pop, spm_wr_fifo_clear;

    FIFO #(.T(spm_req_payload_t), .DEPTH(FIFO_DEPTH)) u_spm_wr_fifo (
        .clk(clk), .reset_n(reset_n),
        .data_in(spm_wr_fifo_din), .push(spm_wr_fifo_push),
        .data_out(spm_wr_fifo_dout), .pop(spm_wr_fifo_pop),
        .empty(spm_wr_fifo_empty), .full(spm_wr_fifo_full),
        .clear(spm_wr_fifo_clear)
    );

    // =========================================================================
    // Internal registers
    // =========================================================================
    logic [31:0] global_ctrl_reg;
    logic [31:0] global_status_reg;
    logic [31:0] plane_en_reg;
    logic [31:0] plane_mode_reg;
    logic [31:0] arb_policy_reg;
    logic [31:0] err_code_reg;
    logic [31:0] err_info0_reg;
    logic [31:0] err_info1_reg;
    logic [31:0] counter_tx_pkt_reg;
    logic [31:0] counter_tx_byte_reg;
    logic [31:0] counter_rx_byte_reg;
    logic [31:0] counter_stall_reg;

    logic run_active_latched;
    logic prev_any_busy_latched;
    logic done_latched;

    // =========================================================================
    // Helper function
    // =========================================================================
    function logic [NOC_ADDR_BITS-1:0] encode_noc_addr(
        input logic [15:0] tag, input logic ultra
    );
        logic [NOC_ADDR_BITS-1:0] val;
        val = '0;
        val[NOC_TAG_BITS-1:0] = tag[NOC_TAG_BITS-1:0];
        val[NOC_TAG_BITS]     = ultra;
        return val;
    endfunction

    // =========================================================================
    // comb_mmio_read
    // =========================================================================
    always_comb begin
        logic [31:0] a;
        logic [31:0] r;
        int          bank;

        a = mmio_addr;
        r = 32'd0;

        if (a >= MMIO_AGU_BASE && a < MMIO_AGU_SIZE) begin
            bank = (a >> 8) & MMIO_AGU_BANK_MASK;
            if (bank < NUM_AGU) r = agu_cfg_rdata[bank];
        end else if (a >= MMIO_GLOBAL_BASE && a <= MMIO_GLOBAL_END) begin
            case (a)
                MMIO_GLOBAL_CTRL:        r = global_ctrl_reg;
                MMIO_GLOBAL_STATUS:      r = global_status_reg;
                MMIO_GLOBAL_PLANE_EN:    r = plane_en_reg;
                MMIO_GLOBAL_PLANE_MODE:  r = plane_mode_reg;
                MMIO_GLOBAL_NUM_PLANES:  r = NUM_AGU;
                MMIO_GLOBAL_PORT_WIDTH:  r = DATA_BITS / 4;
                MMIO_GLOBAL_ARB_POLICY:  r = arb_policy_reg;
                MMIO_GLOBAL_ERR_CODE:    r = err_code_reg;
                MMIO_GLOBAL_ERR_INFO0:   r = err_info0_reg;
                MMIO_GLOBAL_ERR_INFO1:   r = err_info1_reg;
                MMIO_GLOBAL_CNT_TX_PKT:  r = counter_tx_pkt_reg;
                MMIO_GLOBAL_CNT_TX_BYTE: r = counter_tx_byte_reg;
                MMIO_GLOBAL_CNT_RX_BYTE: r = counter_rx_byte_reg;
                MMIO_GLOBAL_CNT_STALL:   r = counter_stall_reg;
                default:                 r = 32'd0;
            endcase
        end

        mmio_rdata = r;
    end

    // =========================================================================
    // comb_read_noc_addr_wait_fifo_data_in
    // =========================================================================
    always_comb begin
        for (int lane = 0; lane < NUM_SEND_PLANES; lane++)
            wait_fifo_din[lane] = encode_noc_addr(agu_gen_tag[lane], agu_gen_ultra[lane]);
    end

    // =========================================================================
    // comb_spm_read_req
    // =========================================================================
    always_comb begin
        logic vld;
        logic en;
        logic room;

        for (int lane = 0; lane < NUM_SEND_PLANES; lane++) begin
            vld  = agu_gen_valid[lane];
            en   = (plane_en_reg[lane] == 1'b1);
            room = !wait_fifo_full[lane];

            if (vld && en && room) begin
                spm_req_valid[lane] = 1'b1;
                spm_req_addr[lane]  = agu_gen_addr[lane][SPM_ADDR_BITS-1:0];
                spm_req_wdata[lane] = '0;
                spm_req_wen[lane]   = 1'b0;
            end else begin
                spm_req_valid[lane] = 1'b0;
                spm_req_addr[lane]  = '0;
                spm_req_wdata[lane] = '0;
                spm_req_wen[lane]   = 1'b0;
            end
        end
    end

    // =========================================================================
    // comb_spm_read_req_push + AGU ready
    // =========================================================================
    always_comb begin
        logic vld;
        logic rdy;

        for (int lane = 0; lane < NUM_SEND_PLANES; lane++) begin
            vld = agu_gen_valid[lane] &&
                  (plane_en_reg[lane] == 1'b1) &&
                  !wait_fifo_full[lane];
            rdy = spm_req_ready[lane];
            if (vld && rdy) begin
                wait_fifo_push[lane] = 1'b1;
                agu_gen_ready[lane]  = 1'b1;
            end else begin
                wait_fifo_push[lane] = 1'b0;
                agu_gen_ready[lane]  = 1'b0;
            end
        end
    end

    // =========================================================================
    // comb_spm_read_resp_ready
    // =========================================================================
    always_comb begin
        logic plane_on;
        logic waiting;
        logic room;
        logic plo_on;
        logic wr_pending;

        for (int lane = 0; lane < NUM_SEND_PLANES; lane++) begin
            plane_on = (plane_en_reg[lane] == 1'b1);
            waiting  = !wait_fifo_empty[lane];
            room     = !noc_req_fifo_full[lane];
            spm_resp_ready[lane] = plane_on && waiting && room;
        end
        plo_on     = (plane_en_reg[RECV_PLANE] == 1'b1);
        wr_pending = !write_addr_fifo_empty || !spm_wr_fifo_empty;
        spm_resp_ready[RECV_PLANE] = plo_on && wr_pending;
    end

    // =========================================================================
    // comb_noc_send_req_fifo_data_in
    // =========================================================================
    always_comb begin
        for (int lane = 0; lane < NUM_SEND_PLANES; lane++) begin
            noc_req_fifo_din[lane].data = spm_resp_rdata[lane];
            noc_req_fifo_din[lane].addr = {{(16-NOC_ADDR_BITS){1'b0}}, wait_fifo_dout[lane]};
            noc_req_fifo_din[lane].mask = {DATA_BITS{1'b1}};
        end
    end

    // =========================================================================
    // comb_spm_read_resp -> pop wait FIFO + push noc_req FIFO
    // =========================================================================
    always_comb begin
        for (int lane = 0; lane < NUM_SEND_PLANES; lane++) begin
            if (spm_resp_valid[lane] && spm_resp_ready[lane]) begin
                wait_fifo_pop[lane]     = 1'b1;
                noc_req_fifo_push[lane] = 1'b1;
            end else begin
                wait_fifo_pop[lane]     = 1'b0;
                noc_req_fifo_push[lane] = 1'b0;
            end
        end
    end

    // =========================================================================
    // comb_noc_req_valid
    // =========================================================================
    always_comb begin
        if (plane_en_reg[0]) begin
            noc_ps_valid = !noc_req_fifo_empty[0];
            noc_ps_data  = noc_req_fifo_dout[0].data;
            noc_ps_addr  = noc_req_fifo_dout[0].addr;
            noc_ps_mask  = noc_req_fifo_dout[0].mask;
        end else begin
            noc_ps_valid = 1'b0;
            noc_ps_data  = '0;
            noc_ps_addr  = '0;
            noc_ps_mask  = '0;
        end
        if (plane_en_reg[1]) begin
            noc_pd_valid = !noc_req_fifo_empty[1];
            noc_pd_data  = noc_req_fifo_dout[1].data;
            noc_pd_addr  = noc_req_fifo_dout[1].addr;
            noc_pd_mask  = noc_req_fifo_dout[1].mask;
        end else begin
            noc_pd_valid = 1'b0;
            noc_pd_data  = '0;
            noc_pd_addr  = '0;
            noc_pd_mask  = '0;
        end
        if (plane_en_reg[2]) begin
            noc_pli_valid = !noc_req_fifo_empty[2];
            noc_pli_data  = noc_req_fifo_dout[2].data;
            noc_pli_addr  = noc_req_fifo_dout[2].addr;
            noc_pli_mask  = noc_req_fifo_dout[2].mask;
        end else begin
            noc_pli_valid = 1'b0;
            noc_pli_data  = '0;
            noc_pli_addr  = '0;
            noc_pli_mask  = '0;
        end
    end

    // =========================================================================
    // comb_noc_req_pop
    // =========================================================================
    always_comb begin
        noc_req_fifo_pop[0] = plane_en_reg[0] && !noc_req_fifo_empty[0] && noc_ps_ready;
        noc_req_fifo_pop[1] = plane_en_reg[1] && !noc_req_fifo_empty[1] && noc_pd_ready;
        noc_req_fifo_pop[2] = plane_en_reg[2] && !noc_req_fifo_empty[2] && noc_pli_ready;
    end

    // =========================================================================
    // comb_noc_plo_req_valid
    // =========================================================================
    always_comb begin
        logic plo_on;
        logic plo_room;

        plo_on   = (plane_en_reg[RECV_PLANE] == 1'b1);
        plo_room = !write_addr_fifo_full;
        if (plo_on && plo_room) begin
            noc_plo_req_valid = agu_gen_valid[RECV_PLANE];
            noc_plo_req_addr  = encode_noc_addr(agu_gen_tag[RECV_PLANE], agu_gen_ultra[RECV_PLANE]);
        end else begin
            noc_plo_req_valid = 1'b0;
            noc_plo_req_addr  = '0;
        end
    end

    // =========================================================================
    // comb_noc_plo_req_push + AGU ready (PLO)
    // =========================================================================
    always_comb begin
        logic plo_on;
        logic plo_room;
        logic plo_vld;
        logic plo_rdy;

        plo_on   = (plane_en_reg[RECV_PLANE] == 1'b1);
        plo_room = !write_addr_fifo_full;
        plo_vld  = agu_gen_valid[RECV_PLANE];
        plo_rdy  = noc_plo_req_ready;

        if (plo_on && plo_room && plo_vld && plo_rdy) begin
            write_addr_fifo_push      = 1'b1;
            agu_gen_ready[RECV_PLANE] = 1'b1;
        end else begin
            write_addr_fifo_push      = 1'b0;
            agu_gen_ready[RECV_PLANE] = 1'b0;
        end
    end

    // =========================================================================
    // comb_noc_plo_resp_ready
    // =========================================================================
    always_comb begin
        logic plo_on;
        logic waiting;
        logic room;

        plo_on  = (plane_en_reg[RECV_PLANE] == 1'b1);
        waiting = !write_addr_fifo_empty;
        room    = !spm_wr_fifo_full;
        noc_plo_resp_ready = plo_on && waiting && room;
    end

    // =========================================================================
    // comb_spm_write_req_fifo_data_in
    // =========================================================================
    always_comb begin
        spm_wr_fifo_din.addr  = write_addr_fifo_dout;
        spm_wr_fifo_din.wdata = noc_plo_resp_data;
        spm_wr_fifo_din.wen   = 1'b1;
    end

    // =========================================================================
    // comb_noc_plo_resp -> pop addr FIFO + push spm_wr FIFO
    // =========================================================================
    always_comb begin
        if (noc_plo_resp_valid && noc_plo_resp_ready) begin
            write_addr_fifo_pop = 1'b1;
            spm_wr_fifo_push    = 1'b1;
        end else begin
            write_addr_fifo_pop = 1'b0;
            spm_wr_fifo_push    = 1'b0;
        end
    end

    // =========================================================================
    // comb_spm_write_req_valid
    // =========================================================================
    always_comb begin
        logic plo_on;
        plo_on = (plane_en_reg[RECV_PLANE] == 1'b1);
        spm_req_valid[RECV_PLANE] = plo_on && !spm_wr_fifo_empty;
        spm_req_addr[RECV_PLANE]  = spm_wr_fifo_dout.addr;
        spm_req_wdata[RECV_PLANE] = spm_wr_fifo_dout.wdata;
        spm_req_wen[RECV_PLANE]   = spm_wr_fifo_dout.wen;
    end

    // =========================================================================
    // comb_spm_write_req_pop
    // =========================================================================
    always_comb begin
        logic plo_on;
        plo_on = (plane_en_reg[RECV_PLANE] == 1'b1);
        spm_wr_fifo_pop = !spm_wr_fifo_empty && spm_req_ready[RECV_PLANE] && plo_on;
    end

    // =========================================================================
    // seq_process
    // =========================================================================
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            global_ctrl_reg      <= '0;
            global_status_reg    <= '0;
            plane_en_reg         <= DEFAULT_PLANE_EN;
            plane_mode_reg       <= '0;
            arb_policy_reg       <= '0;
            err_code_reg         <= '0;
            err_info0_reg        <= '0;
            err_info1_reg        <= '0;
            counter_tx_pkt_reg   <= '0;
            counter_tx_byte_reg  <= '0;
            counter_rx_byte_reg  <= '0;
            counter_stall_reg    <= '0;
            interrupt            <= 1'b0;
            run_active_latched   <= 1'b0;
            prev_any_busy_latched<= 1'b0;
            done_latched         <= 1'b0;

            for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                noc_req_fifo_clear[i] <= 1'b0;
                wait_fifo_clear[i]    <= 1'b0;
            end
            write_addr_fifo_clear <= 1'b0;
            spm_wr_fifo_clear     <= 1'b0;

            for (int i = 0; i < NUM_AGU; i++) begin
                agu_cfg_write[i] <= 1'b0;
                agu_cfg_addr[i]  <= '0;
                agu_cfg_wdata[i] <= '0;
                agu_start[i]     <= 1'b0;
                agu_stop[i]      <= 1'b0;
            end
        end else begin : seq_main
            reg [31:0] a_seq;
            integer    bank_seq;
            reg [7:0]  sub_seq;
            reg        any_busy_seq;
            reg        done_pulse_seq;
            reg [31:0] status_seq;
            integer    stall_inc_seq;
            integer    tx_pkt_inc_seq;
            integer    tx_byte_inc_seq;
            integer    rx_byte_inc_seq;

            // Deassert one-shot signals
            for (int i = 0; i < NUM_AGU; i++) begin
                agu_cfg_write[i] <= 1'b0;
                agu_start[i]     <= 1'b0;
                agu_stop[i]      <= 1'b0;
            end
            for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                noc_req_fifo_clear[i] <= 1'b0;
                wait_fifo_clear[i]    <= 1'b0;
            end
            write_addr_fifo_clear <= 1'b0;
            spm_wr_fifo_clear     <= 1'b0;

            // MMIO routing
            if (mmio_write) begin
                a_seq = mmio_addr;

                // AGU MMIO
                if (a_seq >= MMIO_AGU_BASE && a_seq < MMIO_AGU_SIZE) begin
                    bank_seq = (a_seq >> 8) & MMIO_AGU_BANK_MASK;
                    sub_seq  = a_seq & MMIO_AGU_SUB_MASK;
                    if (bank_seq < NUM_AGU) begin
                        agu_cfg_addr[bank_seq]  <= sub_seq;
                        agu_cfg_wdata[bank_seq] <= mmio_wdata;
                        agu_cfg_write[bank_seq] <= 1'b1;
                        if (sub_seq == MMIO_AGU_CTRL_OFFSET) begin
                            if (mmio_wdata[0]) agu_start[bank_seq] <= 1'b1;
                            if (mmio_wdata[1]) agu_stop[bank_seq]  <= 1'b1;
                        end
                    end
                end

                // Global MMIO
                if (a_seq >= MMIO_GLOBAL_BASE && a_seq <= MMIO_GLOBAL_END) begin
                    case (a_seq)
                        MMIO_GLOBAL_CTRL: begin
                            global_ctrl_reg <= mmio_wdata;
                            if (mmio_wdata[0]) begin
                                err_code_reg  <= '0;
                                err_info0_reg <= '0;
                                err_info1_reg <= '0;
                                for (int i = 0; i < NUM_SEND_PLANES; i++) begin
                                    noc_req_fifo_clear[i] <= 1'b1;
                                    wait_fifo_clear[i]    <= 1'b1;
                                end
                                write_addr_fifo_clear <= 1'b1;
                                spm_wr_fifo_clear     <= 1'b1;
                                run_active_latched    <= 1'b0;
                                done_latched          <= 1'b0;
                                prev_any_busy_latched <= 1'b0;
                            end
                            if (mmio_wdata[1]) begin
                                for (int i = 0; i < NUM_AGU; i++)
                                    agu_start[i] <= 1'b1;
                                run_active_latched <= 1'b1;
                                done_latched       <= 1'b0;
                            end
                            if (mmio_wdata[2]) begin
                                for (int i = 0; i < NUM_AGU; i++)
                                    agu_stop[i] <= 1'b1;
                                run_active_latched <= 1'b0;
                                done_latched       <= 1'b0;
                            end
                        end
                        MMIO_GLOBAL_PLANE_EN:   plane_en_reg   <= mmio_wdata;
                        MMIO_GLOBAL_PLANE_MODE: plane_mode_reg <= mmio_wdata;
                        MMIO_GLOBAL_ARB_POLICY: arb_policy_reg <= mmio_wdata;
                        default: ;
                    endcase
                end
            end

            // Status update
            any_busy_seq = 1'b0;
            for (int i = 0; i < NUM_AGU; i++)
                any_busy_seq = any_busy_seq | agu_busy[i];

            done_pulse_seq = run_active_latched && prev_any_busy_latched && !any_busy_seq;
            if (done_pulse_seq) begin
                done_latched       <= 1'b1;
                run_active_latched <= 1'b0;
            end
            prev_any_busy_latched <= any_busy_seq;

            // Counter logic
            stall_inc_seq   = 0;
            tx_pkt_inc_seq  = 0;
            tx_byte_inc_seq = 0;
            rx_byte_inc_seq = 0;

            if (noc_ps_valid && noc_ps_ready) begin
                tx_pkt_inc_seq  = tx_pkt_inc_seq + 1;
                tx_byte_inc_seq = tx_byte_inc_seq + DATA_BYTES;
            end
            if (noc_pd_valid && noc_pd_ready) begin
                tx_pkt_inc_seq  = tx_pkt_inc_seq + 1;
                tx_byte_inc_seq = tx_byte_inc_seq + DATA_BYTES;
            end
            if (noc_pli_valid && noc_pli_ready) begin
                tx_pkt_inc_seq  = tx_pkt_inc_seq + 1;
                tx_byte_inc_seq = tx_byte_inc_seq + DATA_BYTES;
            end
            if (noc_plo_req_valid && noc_plo_req_ready) begin
                tx_pkt_inc_seq = tx_pkt_inc_seq + 1;
            end
            if (noc_plo_resp_valid && noc_plo_resp_ready) begin
                rx_byte_inc_seq = rx_byte_inc_seq + DATA_BYTES;
            end
            for (int i = 0; i < NUM_AGU; i++) begin
                if (agu_gen_valid[i] && !agu_gen_ready[i])
                    stall_inc_seq = stall_inc_seq + 1;
            end

            // Build status register
            status_seq    = global_status_reg;
            status_seq[1] = any_busy_seq;
            status_seq[2] = done_latched;
            status_seq[4] = (err_code_reg != 0);
            status_seq[0] = !run_active_latched && !any_busy_seq && !done_latched && (err_code_reg == 0);

            if (stall_inc_seq > 0) begin
                counter_stall_reg <= counter_stall_reg + stall_inc_seq;
                status_seq[3] = 1'b1;
            end
            if (tx_pkt_inc_seq > 0)
                counter_tx_pkt_reg <= counter_tx_pkt_reg + tx_pkt_inc_seq;
            if (tx_byte_inc_seq > 0)
                counter_tx_byte_reg <= counter_tx_byte_reg + tx_byte_inc_seq;
            if (rx_byte_inc_seq > 0)
                counter_rx_byte_reg <= counter_rx_byte_reg + rx_byte_inc_seq;

            global_status_reg <= status_seq;
            interrupt <= status_seq[4] | status_seq[2];

            // PLO write response check
            if (spm_resp_valid[RECV_PLANE] && spm_resp_ready[RECV_PLANE]) begin
                if (spm_resp_code[RECV_PLANE] != SPM_OK) begin
                    err_code_reg  <= 32'd3;
                    err_info0_reg <= err_info0_reg | (32'd1 << RECV_PLANE);
                    err_info1_reg <= spm_req_addr[RECV_PLANE];
                end
            end
        end
    end

endmodule
