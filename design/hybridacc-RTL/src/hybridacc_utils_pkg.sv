//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc
// Module Name:   hybridacc_utils_pkg
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Common utility package with type definitions, FP16 arithmetic, and shared constants.
// Dependencies:  None
// Revision:
//   2026/03/28 - Initial version
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
interface vr_if #(type T = logic [31:0]);
    logic valid;
    logic ready;
    T data;

    modport source (
        output valid,
        output data,
        input  ready
    );

    modport sink (
        input  valid,
        input  data,
        output ready
    );

    modport monitor (
        input valid,
        input ready,
        input data
    );
endinterface

package hybridacc_utils_pkg;

    typedef logic [15:0] fp16_t;
    typedef logic [15:0] pe_inst_t;

    localparam logic [31:0] PE_CMD_ADDRESS = 32'h0000_0040;

    localparam int unsigned PE_CMD_OFFSET = 0;
    localparam int unsigned PE_CMD_BITS = 4;

    localparam int unsigned PE_ROUTER_IM_ADDR_OFFSET = 4;
    localparam int unsigned PE_ROUTER_IM_DATA_OFFSET = 16;

    localparam logic [11:0] PE_ROUTER_IM_ADDR_MASK = 12'hFFF;
    localparam logic [15:0] PE_ROUTER_IM_DATA_MASK = 16'hFFFF;

    typedef enum int unsigned {
        DEBUG_LEVEL_NONE = 0,
        DEBUG_LEVEL_PE_COMPONENTS,
        DEBUG_LEVEL_PE_STAGE,
        DEBUG_LEVEL_PE_TOP,
        DEBUG_LEVEL_NOC_COMPONENTS,
        DEBUG_LEVEL_NOC_TOP,
        DEBUG_LEVEL_ALL
    } DebugLevel;

    typedef struct packed {
        logic [3:0][15:0] lanes;
    } v_fp16_t;

    function automatic logic [63:0] v_fp16_to_u64(input v_fp16_t v);
        return {v.lanes[3], v.lanes[2], v.lanes[1], v.lanes[0]};
    endfunction

    function automatic v_fp16_t u64_to_v_fp16(input logic [63:0] value);
        return v_fp16_t'(value);
    endfunction

    typedef struct packed {
        logic [63:0] data;
        logic [15:0] addr;
        logic [63:0] mask;
    } noc_request_t;

    typedef struct packed {
        logic [15:0] addr;
    } noc_addr_req_t;

    // -------------------------------------------------------------------------
    // SPM (Scratchpad Memory) types — matching ESL utils.hpp
    // -------------------------------------------------------------------------
    typedef enum logic [0:0] {
        SPM_OK    = 1'b0,
        SPM_ERROR = 1'b1
    } SPM_RESPONSE_CODE;

    typedef enum logic [1:0] {
        PLANE_PS  = 2'd0,
        PLANE_PD  = 2'd1,
        PLANE_PLI = 2'd2,
        PLANE_PLO = 2'd3
    } PlaneId;

    // -------------------------------------------------------------------------
    // HDDU (HybridDataDeliverUnit) enumerations
    // -------------------------------------------------------------------------
    typedef enum logic [2:0] {
        HDDU_STATUS_IDLE  = 3'd0,
        HDDU_STATUS_BUSY  = 3'd1,
        HDDU_STATUS_DONE  = 3'd2,
        HDDU_STATUS_STALL = 3'd3,
        HDDU_STATUS_ERROR = 3'd4
    } HdduStatusBit;

    typedef enum logic [1:0] {
        HDDU_CTRL_RESET = 2'd0,
        HDDU_CTRL_START = 2'd1,
        HDDU_CTRL_STOP  = 2'd2
    } HdduCtrlBit;

    typedef enum logic [1:0] {
        HDDU_ERR_NONE      = 2'd0,
        HDDU_ERR_AGU_ERROR = 2'd1,
        HDDU_ERR_NOC_ERROR = 2'd2,
        HDDU_ERR_SPM_ERROR = 2'd3
    } HdduErrorCode;

    typedef enum logic [1:0] {
        NOC_CHANNEL_PS  = 2'd0,
        NOC_CHANNEL_PD  = 2'd1,
        NOC_CHANNEL_PLI = 2'd2,
        NOC_CHANNEL_PLO = 2'd3
    } NOC_CHANNELS;

    typedef enum logic [1:0] {
        NOC_OK    = 2'd0,
        NOC_ERROR = 2'd1,
        NOC_NOP   = 2'd2
    } NOC_RESPONSE_STATUS;

    typedef struct packed {
        logic [63:0] data;
        NOC_RESPONSE_STATUS status;
    } noc_response_t;

    typedef struct packed {
        logic [15:0] inst;
        logic        halt;
        logic        nop;
        logic [31:0] func3;
        logic [15:0] imm;
        logic        loop_in;
        logic        loop_end;
        logic        is_swap;
        logic        sys_sdma_act;
        logic        sys_sdma_rst;
        logic        sys_ldma_act;
        logic        sys_ldma_rst;
        logic        sys_rst_pid;
        logic        sys_rst_tid;
        logic        DMA_setaddr;
        logic        DMA_setlen;
        logic        DMA_setloop;
        logic        DMA_setmode;
        logic        LDMA_next;
        logic        DMA_is_sdma;
        logic [31:0] rid3;
        logic [31:0] rid5;
        logic        pd_load;
        logic        pd_load_v;
        logic        tr_en;
        logic        tr_write;
        logic        tr_write_v;
        logic        tr_shift;
        logic        tr_clear_regs;
        logic        tr_use_vcounter;
        logic        tr_incr_vcounter;
        logic        pli_plo_operation;
        logic        pr_en;
        logic        pr_write;
        logic        pr_mode;
        logic        pr_clear_regs;
        logic        pr_use_vcounter;
        logic        pr_incr_vcounter;
        logic        vaddu_en;
        logic [31:0] vaddu_mode;
    } pe_decode_signals_t;

    function automatic pe_decode_signals_t pe_decode_signals_zero();
        return '0;
    endfunction

    typedef enum logic [1:0] {
        PLI_FROM_LN_PLO_TO_LN  = 2'b00,
        PLI_FROM_BUS_PLO_TO_LN = 2'b01,
        PLI_FROM_LN_PLO_TO_BUS = 2'b10,
        PLI_FROM_BUS_PLO_TO_BUS= 2'b11
    } PERouterMode;

    typedef enum logic [3:0] {
        CMD_RESET         = 4'd0,
        CMD_INIT          = 4'd1,
        CMD_LOAD_PROGRAM  = 4'd2,
        CMD_STOP_PE       = 4'd3,
        CMD_START_PE      = 4'd4,
        CMD_NOC_SCAN_CHAIN= 4'd8
    } message_command_t;

    typedef struct packed {
        logic [5:0]  ps_id;
        logic [5:0]  pd_id;
        logic [5:0]  pli_id;
        logic [5:0]  plo_id;
        PERouterMode route_mode;
        logic        enable;
    } ScanChainFormat;

    function automatic ScanChainFormat parse_scan_chain_data(input logic [26:0] data);
        ScanChainFormat format;
        format.ps_id      = data[5:0];
        format.pd_id      = data[11:6];
        format.pli_id     = data[17:12];
        format.plo_id     = data[23:18];
        format.route_mode = PERouterMode'(data[25:24]);
        format.enable     = data[26];
        return format;
    endfunction

    typedef enum logic [1:0] {
        TRACE_PID_PE_ROUTER = 2'd0,
        TRACE_PID_PE        = 2'd1,
        TRACE_PID_MBUS      = 2'd2,
        TRACE_PID_NOC_ROUTER= 2'd3
    } TRACE_PID;

endpackage
