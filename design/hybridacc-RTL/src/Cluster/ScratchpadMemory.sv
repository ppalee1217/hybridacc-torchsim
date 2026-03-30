//-----------------------------------------------------------------------------
// Module Name:   ScratchpadMemory
// Description:   RTL conversion of ESL ScratchpadMemory.hpp
//                4 NoC ports, SRAM bank groups, AXI4-Lite DMA, priority arb,
//                credit-based flow control, skid buffers, PMU counters.
//-----------------------------------------------------------------------------
module ScratchpadMemory
    import hybridacc_utils_pkg::*;
#(
    parameter int unsigned NUM_NOC_PORTS           = 4,
    parameter int unsigned BANKS_PER_GROUP         = 3,
    parameter int unsigned BANK_DATA_WIDTH         = 64,
    parameter int unsigned BANK_DEPTH              = 8192,
    parameter int unsigned SRAM_BANK_LATENCY       = 1,
    parameter int unsigned SRAM_BANK_PIPELINE_DEPTH= 1,
    parameter int unsigned ADDR_WIDTH              = 32,
    parameter int unsigned MAX_OUTSTANDING         = 8,
    parameter int unsigned DMA_MAX_OUTSTANDING     = 8
) (
    input  logic clk,
    input  logic reset_n,
    input  logic pmu_rst_i,

    // Config
    input  logic [7:0]  config_map_i,
    input  logic        config_update_i,
    input  logic        arb_policy_i,

    // NoC slave ports
    input  logic [NUM_NOC_PORTS-1:0]                                      spm_req_valid_i,
    output logic [NUM_NOC_PORTS-1:0]                                      spm_req_ready_o,
    input  logic [NUM_NOC_PORTS-1:0][ADDR_WIDTH-1:0]                      spm_req_addr_i,
    input  logic [NUM_NOC_PORTS-1:0][BANKS_PER_GROUP*BANK_DATA_WIDTH-1:0] spm_req_wdata_i,
    input  logic [NUM_NOC_PORTS-1:0]                                      spm_req_wen_i,

    output logic [NUM_NOC_PORTS-1:0]                                      spm_resp_valid_o,
    input  logic [NUM_NOC_PORTS-1:0]                                      spm_resp_ready_i,
    output logic [NUM_NOC_PORTS-1:0][BANKS_PER_GROUP*BANK_DATA_WIDTH-1:0] spm_resp_rdata_o,
    output SPM_RESPONSE_CODE                                              spm_resp_code_o [NUM_NOC_PORTS],

    // AXI4-Lite DMA slave
    input  logic                         s_axi_awvalid_i,
    output logic                         s_axi_awready_o,
    input  logic [ADDR_WIDTH-1:0]        s_axi_awaddr_i,

    input  logic                         s_axi_wvalid_i,
    output logic                         s_axi_wready_o,
    input  logic [BANK_DATA_WIDTH-1:0]   s_axi_wdata_i,
    input  logic [BANK_DATA_WIDTH/8-1:0] s_axi_wstrb_i,

    output logic                         s_axi_bvalid_o,
    input  logic                         s_axi_bready_i,
    output logic [1:0]                   s_axi_bresp_o,

    input  logic                         s_axi_arvalid_i,
    output logic                         s_axi_arready_o,
    input  logic [ADDR_WIDTH-1:0]        s_axi_araddr_i,

    output logic                         s_axi_rvalid_o,
    input  logic                         s_axi_rready_i,
    output logic [BANK_DATA_WIDTH-1:0]   s_axi_rdata_o,
    output logic [1:0]                   s_axi_rresp_o,

    // PMU
    output logic [63:0]                  pmu_cycle_cnt_o,
    output logic [63:0]                  pmu_port_txn_cnt_o [NUM_NOC_PORTS],
    output logic [63:0]                  pmu_arb_stall_cnt_o,
    output logic [63:0]                  pmu_credit_stall_cnt_o
);

    // -------------------------------------------------------------------------
    // Derived parameters
    // -------------------------------------------------------------------------
    localparam int unsigned NUM_GROUPS         = NUM_NOC_PORTS;
    localparam int unsigned TOTAL_BANKS        = NUM_GROUPS * BANKS_PER_GROUP;
    localparam int unsigned NOC_DATA_WIDTH     = BANKS_PER_GROUP * BANK_DATA_WIDTH;
    localparam int unsigned GROUP_LINEAR_WORDS = BANKS_PER_GROUP * BANK_DEPTH;
    localparam int unsigned GROUP_SPAN_WORDS   = (BANKS_PER_GROUP + 1) * BANK_DEPTH;
    localparam int unsigned BYTES_PER_BANK_WORD= BANK_DATA_WIDTH / 8;
    localparam int unsigned BANK_BYTE_MASK_W   = BANK_DATA_WIDTH / 8;

    localparam int unsigned DMA_FIFO_DEPTH         = DMA_MAX_OUTSTANDING;
    localparam int unsigned GROUP_META_FIFO_DEPTH   = MAX_OUTSTANDING;
    localparam int unsigned PORT_RESP_FIFO_DEPTH    = MAX_OUTSTANDING + 2;

    // -------------------------------------------------------------------------
    // ReadMeta type
    // -------------------------------------------------------------------------
    typedef struct packed {
        logic                   is_dma;
        logic [1:0]             port_id;
        logic                   is_parallel;
        logic [ADDR_WIDTH-1:0]  addr;
    } read_meta_t;

    typedef struct packed {
        logic [ADDR_WIDTH-1:0]        addr;
        logic [BANK_DATA_WIDTH-1:0]   data;
        logic [BANK_BYTE_MASK_W-1:0]  strb;
    } dma_write_req_t;

    typedef struct packed {
        logic [NOC_DATA_WIDTH-1:0]    rdata;
        SPM_RESPONSE_CODE             code;
    } spm_resp_payload_t;

    // =========================================================================
    // SRAM bank signals
    // =========================================================================
    logic [ADDR_WIDTH-1:0]       bank_req_addr   [TOTAL_BANKS];
    logic                        bank_req_valid  [TOTAL_BANKS];
    logic                        bank_req_ready  [TOTAL_BANKS];
    logic [BANK_DATA_WIDTH-1:0]  bank_resp_data  [TOTAL_BANKS];
    logic                        bank_resp_valid [TOTAL_BANKS];
    logic                        bank_resp_ready [TOTAL_BANKS];
    logic                        bank_write_en   [TOTAL_BANKS];
    logic [ADDR_WIDTH-1:0]       bank_write_addr [TOTAL_BANKS];
    logic [BANK_DATA_WIDTH-1:0]  bank_write_data [TOTAL_BANKS];
    logic [BANK_BYTE_MASK_W-1:0] bank_write_mask [TOTAL_BANKS];

    // =========================================================================
    // SRAM bank instances
    // =========================================================================
    genvar gi;
    generate
        for (gi = 0; gi < TOTAL_BANKS; gi++) begin : gen_sram_bank
            SRAM #(
                .DATA_WIDTH_BITS(BANK_DATA_WIDTH),
                .ADDR_WIDTH     (ADDR_WIDTH),
                .SIZE_BYTES     (BANK_DEPTH * BYTES_PER_BANK_WORD),
                .LATENCY        (SRAM_BANK_LATENCY),
                .PIPELINE_DEPTH (SRAM_BANK_PIPELINE_DEPTH)
            ) u_bank (
                .clk       (clk),
                .reset_n   (reset_n),
                .req_addr  (bank_req_addr[gi]),
                .req_valid (bank_req_valid[gi]),
                .req_ready (bank_req_ready[gi]),
                .resp_data (bank_resp_data[gi]),
                .resp_valid(bank_resp_valid[gi]),
                .resp_ready(bank_resp_ready[gi]),
                .write_en  (bank_write_en[gi]),
                .write_addr(bank_write_addr[gi]),
                .write_data(bank_write_data[gi]),
                .write_mask(bank_write_mask[gi])
            );
        end
    endgenerate

    // =========================================================================
    // DMA FIFOs
    // =========================================================================
    logic                   dma_aw_fifo_empty, dma_aw_fifo_full;
    logic [ADDR_WIDTH-1:0]  dma_aw_fifo_din, dma_aw_fifo_dout;
    logic                   dma_aw_fifo_push, dma_aw_fifo_pop, dma_aw_fifo_clear;

    FIFO #(.T(logic [ADDR_WIDTH-1:0]), .DEPTH(DMA_FIFO_DEPTH)) u_dma_aw_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(dma_aw_fifo_din), .push(dma_aw_fifo_push),
        .data_out(dma_aw_fifo_dout), .pop(dma_aw_fifo_pop), .empty(dma_aw_fifo_empty),
        .full(dma_aw_fifo_full), .clear(dma_aw_fifo_clear)
    );

    logic                        dma_w_data_fifo_empty, dma_w_data_fifo_full;
    logic [BANK_DATA_WIDTH-1:0]  dma_w_data_fifo_din, dma_w_data_fifo_dout;
    logic                        dma_w_data_fifo_push, dma_w_data_fifo_pop, dma_w_data_fifo_clear;

    FIFO #(.T(logic [BANK_DATA_WIDTH-1:0]), .DEPTH(DMA_FIFO_DEPTH)) u_dma_w_data_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(dma_w_data_fifo_din), .push(dma_w_data_fifo_push),
        .data_out(dma_w_data_fifo_dout), .pop(dma_w_data_fifo_pop), .empty(dma_w_data_fifo_empty),
        .full(dma_w_data_fifo_full), .clear(dma_w_data_fifo_clear)
    );

    logic                        dma_w_strb_fifo_empty, dma_w_strb_fifo_full;
    logic [BANK_BYTE_MASK_W-1:0] dma_w_strb_fifo_din, dma_w_strb_fifo_dout;
    logic                        dma_w_strb_fifo_push, dma_w_strb_fifo_pop, dma_w_strb_fifo_clear;

    FIFO #(.T(logic [BANK_BYTE_MASK_W-1:0]), .DEPTH(DMA_FIFO_DEPTH)) u_dma_w_strb_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(dma_w_strb_fifo_din), .push(dma_w_strb_fifo_push),
        .data_out(dma_w_strb_fifo_dout), .pop(dma_w_strb_fifo_pop), .empty(dma_w_strb_fifo_empty),
        .full(dma_w_strb_fifo_full), .clear(dma_w_strb_fifo_clear)
    );

    logic                  dma_write_req_fifo_empty, dma_write_req_fifo_full;
    dma_write_req_t        dma_write_req_fifo_din, dma_write_req_fifo_dout;
    logic                  dma_write_req_fifo_push, dma_write_req_fifo_pop, dma_write_req_fifo_clear;

    FIFO #(.T(dma_write_req_t), .DEPTH(DMA_FIFO_DEPTH)) u_dma_write_req_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(dma_write_req_fifo_din), .push(dma_write_req_fifo_push),
        .data_out(dma_write_req_fifo_dout), .pop(dma_write_req_fifo_pop), .empty(dma_write_req_fifo_empty),
        .full(dma_write_req_fifo_full), .clear(dma_write_req_fifo_clear)
    );

    logic                   dma_read_req_fifo_empty, dma_read_req_fifo_full;
    logic [ADDR_WIDTH-1:0]  dma_read_req_fifo_din, dma_read_req_fifo_dout;
    logic                   dma_read_req_fifo_push, dma_read_req_fifo_pop, dma_read_req_fifo_clear;

    FIFO #(.T(logic [ADDR_WIDTH-1:0]), .DEPTH(DMA_FIFO_DEPTH)) u_dma_read_req_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(dma_read_req_fifo_din), .push(dma_read_req_fifo_push),
        .data_out(dma_read_req_fifo_dout), .pop(dma_read_req_fifo_pop), .empty(dma_read_req_fifo_empty),
        .full(dma_read_req_fifo_full), .clear(dma_read_req_fifo_clear)
    );

    logic                        dma_read_resp_fifo_empty, dma_read_resp_fifo_full;
    logic [BANK_DATA_WIDTH-1:0]  dma_read_resp_fifo_din, dma_read_resp_fifo_dout;
    logic                        dma_read_resp_fifo_push, dma_read_resp_fifo_pop, dma_read_resp_fifo_clear;

    FIFO #(.T(logic [BANK_DATA_WIDTH-1:0]), .DEPTH(DMA_FIFO_DEPTH)) u_dma_read_resp_fifo (
        .clk(clk), .reset_n(reset_n), .data_in(dma_read_resp_fifo_din), .push(dma_read_resp_fifo_push),
        .data_out(dma_read_resp_fifo_dout), .pop(dma_read_resp_fifo_pop), .empty(dma_read_resp_fifo_empty),
        .full(dma_read_resp_fifo_full), .clear(dma_read_resp_fifo_clear)
    );

    // =========================================================================
    // Per-group read-metadata FIFOs
    // =========================================================================
    logic               group_meta_fifo_empty [NUM_GROUPS];
    logic               group_meta_fifo_full  [NUM_GROUPS];
    read_meta_t         group_meta_fifo_din   [NUM_GROUPS];
    read_meta_t         group_meta_fifo_dout  [NUM_GROUPS];
    logic               group_meta_fifo_push  [NUM_GROUPS];
    logic               group_meta_fifo_pop   [NUM_GROUPS];
    logic               group_meta_fifo_clear [NUM_GROUPS];

    generate
        for (gi = 0; gi < NUM_GROUPS; gi++) begin : gen_group_meta_fifo
            FIFO #(.T(read_meta_t), .DEPTH(GROUP_META_FIFO_DEPTH)) u_group_meta_fifo (
                .clk(clk), .reset_n(reset_n),
                .data_in(group_meta_fifo_din[gi]), .push(group_meta_fifo_push[gi]),
                .data_out(group_meta_fifo_dout[gi]), .pop(group_meta_fifo_pop[gi]),
                .empty(group_meta_fifo_empty[gi]), .full(group_meta_fifo_full[gi]),
                .clear(group_meta_fifo_clear[gi])
            );
        end
    endgenerate

    // =========================================================================
    // Per-port response FIFOs
    // =========================================================================
    logic               port_resp_fifo_empty [NUM_NOC_PORTS];
    logic               port_resp_fifo_full  [NUM_NOC_PORTS];
    spm_resp_payload_t  port_resp_fifo_din   [NUM_NOC_PORTS];
    spm_resp_payload_t  port_resp_fifo_dout  [NUM_NOC_PORTS];
    logic               port_resp_fifo_push  [NUM_NOC_PORTS];
    logic               port_resp_fifo_pop   [NUM_NOC_PORTS];
    logic               port_resp_fifo_clear [NUM_NOC_PORTS];

    generate
        for (gi = 0; gi < NUM_NOC_PORTS; gi++) begin : gen_port_resp_fifo
            FIFO #(.T(spm_resp_payload_t), .DEPTH(PORT_RESP_FIFO_DEPTH)) u_port_resp_fifo (
                .clk(clk), .reset_n(reset_n),
                .data_in(port_resp_fifo_din[gi]), .push(port_resp_fifo_push[gi]),
                .data_out(port_resp_fifo_dout[gi]), .pop(port_resp_fifo_pop[gi]),
                .empty(port_resp_fifo_empty[gi]), .full(port_resp_fifo_full[gi]),
                .clear(port_resp_fifo_clear[gi])
            );
        end
    endgenerate

    // =========================================================================
    // State registers
    // =========================================================================
    logic                      skid_valid_reg  [NUM_NOC_PORTS];
    logic [ADDR_WIDTH-1:0]     skid_addr_reg   [NUM_NOC_PORTS];
    logic [NOC_DATA_WIDTH-1:0] skid_wdata_reg  [NUM_NOC_PORTS];
    logic                      skid_wen_reg    [NUM_NOC_PORTS];

    logic [7:0]            credit_cnt_reg  [NUM_NOC_PORTS];
    logic [1:0]            active_map_reg  [NUM_NOC_PORTS];
    logic [7:0]            dma_b_pending_cnt_reg;
    logic [7:0]            dma_rd_inflight_cnt_reg;

    logic [63:0]           pmu_cycle_cnt_reg;
    logic [63:0]           pmu_port_txn_cnt_reg [NUM_NOC_PORTS];
    logic [63:0]           pmu_arb_stall_cnt_reg;
    logic [63:0]           pmu_credit_stall_cnt_reg;

    // =========================================================================
    // Intermediate combinational signals
    // =========================================================================
    logic               port_req_fire     [NUM_NOC_PORTS];
    logic               port_req_is_write [NUM_NOC_PORTS];
    logic               wr_resp_push      [NUM_NOC_PORTS];
    spm_resp_payload_t  wr_resp_data      [NUM_NOC_PORTS];
    logic               rd_resp_push      [NUM_NOC_PORTS];
    spm_resp_payload_t  rd_resp_data      [NUM_NOC_PORTS];

    logic               dma_write_fire;
    logic               dma_read_fire;
    logic               dma_read_merge_fire;
    logic [BANK_DATA_WIDTH-1:0] dma_read_merge_data;
    logic               credit_stall;
    logic               arb_stall;

    // =========================================================================
    // comb_spm_req_ready
    // =========================================================================
    always_comb begin
        for (int p = 0; p < NUM_NOC_PORTS; p++)
            spm_req_ready_o[p] = !skid_valid_reg[p];
    end

    // =========================================================================
    // comb_dma_chan_ready
    // =========================================================================
    always_comb begin
        s_axi_awready_o = !dma_aw_fifo_full;
        s_axi_wready_o  = !dma_w_data_fifo_full;
        s_axi_arready_o = !dma_read_req_fifo_full &&
                           !dma_read_resp_fifo_full &&
                           (dma_rd_inflight_cnt_reg < DMA_MAX_OUTSTANDING);
    end

    // =========================================================================
    // comb_dma_chan_push
    // =========================================================================
    always_comb begin
        logic ar_ok;

        dma_aw_fifo_push = s_axi_awvalid_i && !dma_aw_fifo_full;
        dma_aw_fifo_din  = s_axi_awaddr_i;

        dma_w_data_fifo_push = s_axi_wvalid_i && !dma_w_data_fifo_full;
        dma_w_data_fifo_din  = s_axi_wdata_i;

        dma_w_strb_fifo_push = s_axi_wvalid_i && !dma_w_strb_fifo_full;
        dma_w_strb_fifo_din  = s_axi_wstrb_i;

        ar_ok = !dma_read_req_fifo_full && !dma_read_resp_fifo_full &&
                (dma_rd_inflight_cnt_reg < DMA_MAX_OUTSTANDING);
        dma_read_req_fifo_push = s_axi_arvalid_i && ar_ok;
        dma_read_req_fifo_din  = s_axi_araddr_i;
    end

    // =========================================================================
    // comb_dma_aw_w_merge
    // =========================================================================
    always_comb begin
        logic can_merge;
        can_merge = !dma_aw_fifo_empty && !dma_w_data_fifo_empty &&
                    !dma_w_strb_fifo_empty && !dma_write_req_fifo_full;

        dma_aw_fifo_pop         = can_merge;
        dma_w_data_fifo_pop     = can_merge;
        dma_w_strb_fifo_pop     = can_merge;
        dma_write_req_fifo_push = can_merge;

        dma_write_req_fifo_din = '0;
        if (can_merge) begin
            dma_write_req_fifo_din.addr = dma_aw_fifo_dout;
            dma_write_req_fifo_din.data = dma_w_data_fifo_dout;
            dma_write_req_fifo_din.strb = dma_w_strb_fifo_dout;
        end
    end

    // =========================================================================
    // comb_bank_req_arb — priority arbitration
    // =========================================================================
    always_comb begin
        // Hoisted temporaries
        logic                      group_busy [NUM_GROUPS];
        logic                      local_credit_stall, local_arb_stall;
        logic [ADDR_WIDTH-1:0]     req_addr;
        logic [NOC_DATA_WIDTH-1:0] req_wdata;
        logic                      req_wen;
        logic                      port_valid;
        int unsigned               grp_id;
        logic [31:0]               laddr;
        logic                      is_par;
        int unsigned               base_bank;
        logic [31:0]               row;
        logic                      all_ready;
        int unsigned               bidx;
        logic [31:0]               dma_gwaddr;
        int unsigned               dma_grp;
        int unsigned               dma_lidx;
        int unsigned               dma_bidx;
        logic [31:0]               dma_row;

        // Defaults
        local_credit_stall = 1'b0;
        local_arb_stall    = 1'b0;
        for (int b = 0; b < TOTAL_BANKS; b++) begin
            bank_req_valid[b]  = 1'b0;
            bank_req_addr[b]   = '0;
            bank_write_en[b]   = 1'b0;
            bank_write_addr[b] = '0;
            bank_write_data[b] = '0;
            bank_write_mask[b] = '0;
        end
        for (int g = 0; g < NUM_GROUPS; g++) begin
            group_meta_fifo_push[g] = 1'b0;
            group_meta_fifo_din[g]  = '0;
            group_busy[g]           = 1'b0;
        end
        for (int p = 0; p < NUM_NOC_PORTS; p++) begin
            port_req_fire[p]     = 1'b0;
            port_req_is_write[p] = 1'b0;
            wr_resp_push[p]      = 1'b0;
            wr_resp_data[p]      = '0;
        end
        dma_write_fire         = 1'b0;
        dma_read_fire          = 1'b0;
        dma_write_req_fifo_pop = 1'b0;
        dma_read_req_fifo_pop  = 1'b0;

        // --- NoC ports (static priority: 0 = highest) ---
        for (int p = 0; p < NUM_NOC_PORTS; p++) begin
            port_valid = 1'b0;
            req_addr   = '0;
            req_wdata  = '0;
            req_wen    = 1'b0;

            if (skid_valid_reg[p]) begin
                req_addr  = skid_addr_reg[p];
                req_wdata = skid_wdata_reg[p];
                req_wen   = skid_wen_reg[p];
                port_valid= 1'b1;
            end else if (spm_req_valid_i[p]) begin
                req_addr  = spm_req_addr_i[p];
                req_wdata = spm_req_wdata_i[p];
                req_wen   = spm_req_wen_i[p];
                port_valid= 1'b1;
            end

            if (port_valid) begin
                grp_id    = active_map_reg[p];
                laddr     = req_addr;
                is_par    = (laddr >= GROUP_LINEAR_WORDS);
                base_bank = grp_id * BANKS_PER_GROUP;

                if (!req_wen && credit_cnt_reg[p] == 0) begin
                    local_credit_stall = 1'b1;
                end else if (grp_id >= NUM_GROUPS || group_busy[grp_id] ||
                             (!req_wen && group_meta_fifo_full[grp_id])) begin
                    local_arb_stall = 1'b1;
                end else begin
                    if (is_par) begin
                        row = laddr - GROUP_LINEAR_WORDS;
                        if (req_wen) begin
                            for (int k = 0; k < BANKS_PER_GROUP; k++) begin
                                bank_write_en[base_bank+k]   = 1'b1;
                                bank_write_addr[base_bank+k] = row * BYTES_PER_BANK_WORD;
                                bank_write_data[base_bank+k] = req_wdata[k*BANK_DATA_WIDTH +: BANK_DATA_WIDTH];
                                bank_write_mask[base_bank+k] = {BANK_BYTE_MASK_W{1'b1}};
                            end
                        end else begin
                            all_ready = 1'b1;
                            for (int k = 0; k < BANKS_PER_GROUP; k++)
                                if (!bank_req_ready[base_bank+k]) all_ready = 1'b0;
                            if (!all_ready) begin
                                local_arb_stall = 1'b1;
                            end else begin
                                for (int k = 0; k < BANKS_PER_GROUP; k++) begin
                                    bank_req_valid[base_bank+k] = 1'b1;
                                    bank_req_addr[base_bank+k]  = row * BYTES_PER_BANK_WORD;
                                end
                                group_meta_fifo_push[grp_id] = 1'b1;
                                group_meta_fifo_din[grp_id].is_dma      = 1'b0;
                                group_meta_fifo_din[grp_id].port_id     = p[1:0];
                                group_meta_fifo_din[grp_id].is_parallel = 1'b1;
                                group_meta_fifo_din[grp_id].addr        = laddr;
                            end
                        end
                    end else begin
                        bidx = (laddr / BANK_DEPTH) + base_bank;
                        row  = laddr % BANK_DEPTH;
                        if (req_wen) begin
                            bank_write_en[bidx]   = 1'b1;
                            bank_write_addr[bidx] = row * BYTES_PER_BANK_WORD;
                            bank_write_data[bidx] = req_wdata[BANK_DATA_WIDTH-1:0];
                            bank_write_mask[bidx] = {BANK_BYTE_MASK_W{1'b1}};
                        end else begin
                            if (!bank_req_ready[bidx]) begin
                                local_arb_stall = 1'b1;
                            end else begin
                                bank_req_valid[bidx] = 1'b1;
                                bank_req_addr[bidx]  = row * BYTES_PER_BANK_WORD;
                                group_meta_fifo_push[grp_id] = 1'b1;
                                group_meta_fifo_din[grp_id].is_dma      = 1'b0;
                                group_meta_fifo_din[grp_id].port_id     = p[1:0];
                                group_meta_fifo_din[grp_id].is_parallel = 1'b0;
                                group_meta_fifo_din[grp_id].addr        = laddr;
                            end
                        end
                    end

                    // Immediate write response
                    if (req_wen) begin
                        wr_resp_push[p]      = 1'b1;
                        wr_resp_data[p].rdata = '0;
                        wr_resp_data[p].code  = SPM_OK;
                    end

                    group_busy[grp_id]    = 1'b1;
                    port_req_fire[p]      = 1'b1;
                    port_req_is_write[p]  = req_wen;
                end
            end
        end

        // --- DMA write (after NoC ports) ---
        if (!dma_write_req_fifo_empty && dma_b_pending_cnt_reg < DMA_MAX_OUTSTANDING) begin
            dma_gwaddr = dma_write_req_fifo_dout.addr / BYTES_PER_BANK_WORD;
            dma_grp    = dma_gwaddr / GROUP_SPAN_WORDS;
            if (dma_grp < NUM_GROUPS && !group_busy[dma_grp]) begin
                dma_lidx = dma_gwaddr % GROUP_SPAN_WORDS;
                dma_bidx = (dma_lidx / BANK_DEPTH) + dma_grp * BANKS_PER_GROUP;
                dma_row  = dma_lidx % BANK_DEPTH;

                bank_write_en[dma_bidx]   = 1'b1;
                bank_write_addr[dma_bidx] = dma_row * BYTES_PER_BANK_WORD;
                bank_write_data[dma_bidx] = dma_write_req_fifo_dout.data;
                bank_write_mask[dma_bidx] = dma_write_req_fifo_dout.strb;

                dma_write_req_fifo_pop = 1'b1;
                dma_write_fire         = 1'b1;
                group_busy[dma_grp]    = 1'b1;
            end
        end

        // --- DMA read (lowest priority) ---
        if (!dma_read_req_fifo_empty && !dma_read_resp_fifo_full &&
            dma_rd_inflight_cnt_reg < DMA_MAX_OUTSTANDING) begin
            dma_gwaddr = dma_read_req_fifo_dout / BYTES_PER_BANK_WORD;
            dma_grp    = dma_gwaddr / GROUP_SPAN_WORDS;
            if (dma_grp < NUM_GROUPS && !group_busy[dma_grp] && !group_meta_fifo_full[dma_grp]) begin
                dma_lidx = dma_gwaddr % GROUP_SPAN_WORDS;
                dma_bidx = (dma_lidx / BANK_DEPTH) + dma_grp * BANKS_PER_GROUP;
                dma_row  = dma_lidx % BANK_DEPTH;

                if (bank_req_ready[dma_bidx]) begin
                    bank_req_valid[dma_bidx] = 1'b1;
                    bank_req_addr[dma_bidx]  = dma_row * BYTES_PER_BANK_WORD;

                    group_meta_fifo_push[dma_grp] = 1'b1;
                    group_meta_fifo_din[dma_grp].is_dma      = 1'b1;
                    group_meta_fifo_din[dma_grp].port_id     = 2'd0;
                    group_meta_fifo_din[dma_grp].is_parallel = 1'b0;
                    group_meta_fifo_din[dma_grp].addr        = dma_lidx;

                    dma_read_req_fifo_pop = 1'b1;
                    dma_read_fire         = 1'b1;
                    group_busy[dma_grp]   = 1'b1;
                end
            end
        end

        credit_stall = local_credit_stall;
        arb_stall    = local_arb_stall;
    end

    // =========================================================================
    // comb_resp_merge
    // =========================================================================
    always_comb begin
        read_meta_t            resp_meta;
        int unsigned           resp_bbase;
        logic                  resp_all_avail;
        int unsigned           resp_bidx;
        int unsigned           resp_p_id;
        logic [NOC_DATA_WIDTH-1:0] resp_rdata;

        for (int p = 0; p < NUM_NOC_PORTS; p++) begin
            rd_resp_push[p] = 1'b0;
            rd_resp_data[p] = '0;
        end
        dma_read_merge_fire = 1'b0;
        dma_read_merge_data = '0;
        for (int g = 0; g < NUM_GROUPS; g++)
            group_meta_fifo_pop[g] = 1'b0;

        for (int g = 0; g < NUM_GROUPS; g++) begin
            if (!group_meta_fifo_empty[g]) begin
                resp_meta      = group_meta_fifo_dout[g];
                resp_bbase     = g * BANKS_PER_GROUP;
                resp_all_avail = 1'b1;

                if (!resp_meta.is_parallel) begin
                    resp_bidx = (resp_meta.addr / BANK_DEPTH) + resp_bbase;
                    if (!bank_resp_valid[resp_bidx]) resp_all_avail = 1'b0;
                end else begin
                    for (int k = 0; k < BANKS_PER_GROUP; k++)
                        if (!bank_resp_valid[resp_bbase + k]) resp_all_avail = 1'b0;
                end

                if (resp_all_avail) begin
                    if (resp_meta.is_dma) begin
                        if (!dma_read_resp_fifo_full) begin
                            resp_bidx = (resp_meta.addr / BANK_DEPTH) + resp_bbase;
                            dma_read_merge_data = bank_resp_data[resp_bidx];
                            dma_read_merge_fire = 1'b1;
                            group_meta_fifo_pop[g] = 1'b1;
                        end
                    end else begin
                        resp_p_id = resp_meta.port_id;
                        if (!port_resp_fifo_full[resp_p_id]) begin
                            resp_rdata = '0;
                            if (!resp_meta.is_parallel) begin
                                resp_bidx = (resp_meta.addr / BANK_DEPTH) + resp_bbase;
                                resp_rdata[BANK_DATA_WIDTH-1:0] = bank_resp_data[resp_bidx];
                            end else begin
                                for (int k = 0; k < BANKS_PER_GROUP; k++)
                                    resp_rdata[k*BANK_DATA_WIDTH +: BANK_DATA_WIDTH] = bank_resp_data[resp_bbase + k];
                            end
                            rd_resp_push[resp_p_id]      = 1'b1;
                            rd_resp_data[resp_p_id].rdata = resp_rdata;
                            rd_resp_data[resp_p_id].code  = SPM_OK;
                            group_meta_fifo_pop[g]        = 1'b1;
                        end
                    end
                end
            end
        end
    end

    // =========================================================================
    // comb_port_resp_fifo_ctrl
    // =========================================================================
    always_comb begin
        for (int p = 0; p < NUM_NOC_PORTS; p++) begin
            if (wr_resp_push[p]) begin
                port_resp_fifo_push[p] = 1'b1;
                port_resp_fifo_din[p]  = wr_resp_data[p];
            end else if (rd_resp_push[p]) begin
                port_resp_fifo_push[p] = 1'b1;
                port_resp_fifo_din[p]  = rd_resp_data[p];
            end else begin
                port_resp_fifo_push[p] = 1'b0;
                port_resp_fifo_din[p]  = '0;
            end
        end
    end

    // =========================================================================
    // comb_spm_resp_output
    // =========================================================================
    always_comb begin
        for (int p = 0; p < NUM_NOC_PORTS; p++) begin
            spm_resp_valid_o[p]  = !port_resp_fifo_empty[p];
            spm_resp_rdata_o[p]  = port_resp_fifo_dout[p].rdata;
            spm_resp_code_o[p]   = port_resp_fifo_dout[p].code;
        end
    end

    // =========================================================================
    // comb_port_resp_fifo_pop
    // =========================================================================
    always_comb begin
        for (int p = 0; p < NUM_NOC_PORTS; p++)
            port_resp_fifo_pop[p] = !port_resp_fifo_empty[p] && spm_resp_ready_i[p];
    end

    // =========================================================================
    // comb_dma_resp_b (AXI B channel)
    // =========================================================================
    always_comb begin
        s_axi_bvalid_o = (dma_b_pending_cnt_reg > 0);
        s_axi_bresp_o  = 2'b00;
    end

    // =========================================================================
    // comb_dma_resp_r (AXI R channel)
    // =========================================================================
    always_comb begin
        s_axi_rvalid_o = !dma_read_resp_fifo_empty;
        s_axi_rdata_o  = dma_read_resp_fifo_empty ? '0 : dma_read_resp_fifo_dout;
        s_axi_rresp_o  = 2'b00;
    end

    // =========================================================================
    // comb_dma_read_resp_fifo push/pop
    // =========================================================================
    always_comb begin
        dma_read_resp_fifo_push = dma_read_merge_fire;
        dma_read_resp_fifo_din  = dma_read_merge_data;
        dma_read_resp_fifo_pop  = !dma_read_resp_fifo_empty && s_axi_rready_i;
    end

    // =========================================================================
    // comb_bank_resp_ready
    // =========================================================================
    always_comb begin
        read_meta_t rdy_meta;
        int unsigned rdy_bbase;
        int unsigned rdy_k;

        for (int b = 0; b < TOTAL_BANKS; b++)
            bank_resp_ready[b] = 1'b0;

        for (int g = 0; g < NUM_GROUPS; g++) begin
            if (group_meta_fifo_pop[g] && !group_meta_fifo_empty[g]) begin
                rdy_meta  = group_meta_fifo_dout[g];
                rdy_bbase = g * BANKS_PER_GROUP;

                if (rdy_meta.is_parallel) begin
                    for (int k = 0; k < BANKS_PER_GROUP; k++)
                        bank_resp_ready[rdy_bbase + k] = 1'b1;
                end else begin
                    rdy_k = rdy_meta.addr / BANK_DEPTH;
                    if (rdy_k < BANKS_PER_GROUP)
                        bank_resp_ready[rdy_bbase + rdy_k] = 1'b1;
                end
            end
        end
    end

    // =========================================================================
    // pmu_output_process
    // =========================================================================
    always_comb begin
        pmu_cycle_cnt_o        = pmu_cycle_cnt_reg;
        pmu_arb_stall_cnt_o    = pmu_arb_stall_cnt_reg;
        pmu_credit_stall_cnt_o = pmu_credit_stall_cnt_reg;
        for (int p = 0; p < NUM_NOC_PORTS; p++)
            pmu_port_txn_cnt_o[p] = pmu_port_txn_cnt_reg[p];
    end

    // =========================================================================
    // seq_process
    // =========================================================================
    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            for (int p = 0; p < NUM_NOC_PORTS; p++) begin
                skid_valid_reg[p]       <= 1'b0;
                skid_addr_reg[p]        <= '0;
                skid_wdata_reg[p]       <= '0;
                skid_wen_reg[p]         <= 1'b0;
                credit_cnt_reg[p]       <= MAX_OUTSTANDING[7:0];
                active_map_reg[p]       <= p[1:0];
                pmu_port_txn_cnt_reg[p] <= '0;
                port_resp_fifo_clear[p] <= 1'b1;
            end
            for (int g = 0; g < NUM_GROUPS; g++)
                group_meta_fifo_clear[g] <= 1'b1;

            dma_b_pending_cnt_reg    <= '0;
            dma_rd_inflight_cnt_reg  <= '0;
            pmu_cycle_cnt_reg        <= '0;
            pmu_arb_stall_cnt_reg    <= '0;
            pmu_credit_stall_cnt_reg <= '0;

            dma_aw_fifo_clear        <= 1'b1;
            dma_w_data_fifo_clear    <= 1'b1;
            dma_w_strb_fifo_clear    <= 1'b1;
            dma_write_req_fifo_clear <= 1'b1;
            dma_read_req_fifo_clear  <= 1'b1;
            dma_read_resp_fifo_clear <= 1'b1;
        end else begin
            // Deassert all clears
            dma_aw_fifo_clear        <= 1'b0;
            dma_w_data_fifo_clear    <= 1'b0;
            dma_w_strb_fifo_clear    <= 1'b0;
            dma_write_req_fifo_clear <= 1'b0;
            dma_read_req_fifo_clear  <= 1'b0;
            dma_read_resp_fifo_clear <= 1'b0;
            for (int g = 0; g < NUM_GROUPS; g++)
                group_meta_fifo_clear[g] <= 1'b0;
            for (int p = 0; p < NUM_NOC_PORTS; p++)
                port_resp_fifo_clear[p] <= 1'b0;

            // PMU cycle / reset
            if (pmu_rst_i) begin
                pmu_cycle_cnt_reg        <= '0;
                pmu_arb_stall_cnt_reg    <= '0;
                pmu_credit_stall_cnt_reg <= '0;
                for (int p = 0; p < NUM_NOC_PORTS; p++)
                    pmu_port_txn_cnt_reg[p] <= '0;
            end else begin
                pmu_cycle_cnt_reg <= pmu_cycle_cnt_reg + 64'd1;
            end

            // Config update
            if (config_update_i) begin : seq_cfg_update
                reg [7:0]  map_val;
                reg [3:0]  used;
                reg        ok;
                integer    gg;
                map_val = config_map_i;
                used = 4'b0;
                ok = 1'b1;
                for (gg = 0; gg < NUM_NOC_PORTS; gg = gg + 1) begin
                    if ((map_val >> (gg * 2)) & 2'h3 >= NUM_GROUPS) ok = 1'b0;
                    if (used[(map_val >> (gg * 2)) & 2'h3]) ok = 1'b0;
                    used[(map_val >> (gg * 2)) & 2'h3] = 1'b1;
                end
                if (ok) begin
                    for (gg = 0; gg < NUM_NOC_PORTS; gg = gg + 1)
                        active_map_reg[gg] <= (map_val >> (gg * 2)) & 2'h3;
                end
            end

            // Skid buffer update
            for (int p = 0; p < NUM_NOC_PORTS; p++) begin
                if (port_req_fire[p] && skid_valid_reg[p]) begin
                    skid_valid_reg[p] <= 1'b0;
                end else if (spm_req_valid_i[p] && !skid_valid_reg[p] && !port_req_fire[p]) begin
                    skid_valid_reg[p]  <= 1'b1;
                    skid_addr_reg[p]   <= spm_req_addr_i[p];
                    skid_wdata_reg[p]  <= spm_req_wdata_i[p];
                    skid_wen_reg[p]    <= spm_req_wen_i[p];
                end
            end

            // Credit counter update
            for (int p = 0; p < NUM_NOC_PORTS; p++) begin
                if (port_req_fire[p] && !port_req_is_write[p] && !rd_resp_push[p])
                    credit_cnt_reg[p] <= credit_cnt_reg[p] - 8'd1;
                if (!(port_req_fire[p] && !port_req_is_write[p]) && rd_resp_push[p])
                    credit_cnt_reg[p] <= credit_cnt_reg[p] + 8'd1;
            end

            // PMU transaction counters
            for (int p = 0; p < NUM_NOC_PORTS; p++) begin
                if (port_req_fire[p])
                    pmu_port_txn_cnt_reg[p] <= pmu_port_txn_cnt_reg[p] + 64'd1;
            end

            // PMU stall counters
            if (credit_stall)
                pmu_credit_stall_cnt_reg <= pmu_credit_stall_cnt_reg + 64'd1;
            if (arb_stall)
                pmu_arb_stall_cnt_reg <= pmu_arb_stall_cnt_reg + 64'd1;

            // DMA B-pending counter
            if (dma_write_fire && !(s_axi_bvalid_o && s_axi_bready_i))
                dma_b_pending_cnt_reg <= dma_b_pending_cnt_reg + 8'd1;
            if (!dma_write_fire && (s_axi_bvalid_o && s_axi_bready_i) && dma_b_pending_cnt_reg > 0)
                dma_b_pending_cnt_reg <= dma_b_pending_cnt_reg - 8'd1;

            // DMA read in-flight counter
            if (dma_read_fire && !dma_read_merge_fire)
                dma_rd_inflight_cnt_reg <= dma_rd_inflight_cnt_reg + 8'd1;
            if (!dma_read_fire && dma_read_merge_fire && dma_rd_inflight_cnt_reg > 0)
                dma_rd_inflight_cnt_reg <= dma_rd_inflight_cnt_reg - 8'd1;
        end
    end

endmodule
