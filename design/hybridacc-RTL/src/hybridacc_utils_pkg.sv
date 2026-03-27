`ifndef HYBRIDACC_UTILS_PKG_SV
`define HYBRIDACC_UTILS_PKG_SV

`ifndef DEBUG_MSG
`define DEBUG_MSG(msg, level)
`endif

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
        logic [63:0] result;
        result = 64'h0;
        for (int i = 0; i < 4; i++) begin
            result[(i * 16) +: 16] = v.lanes[i];
        end
        return result;
    endfunction

    function automatic v_fp16_t u64_to_v_fp16(input logic [63:0] value);
        v_fp16_t result;
        for (int i = 0; i < 4; i++) begin
            result.lanes[i] = value[(i * 16) +: 16];
        end
        return result;
    endfunction

    typedef struct packed {
        logic [63:0] data;
        logic [15:0] addr;
        logic [63:0] mask;
    } noc_request_t;

    typedef struct packed {
        logic [15:0] addr;
    } noc_addr_req_t;

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
        pe_decode_signals_t s;
        s = '0;
        return s;
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

    function automatic ScanChainFormat parse_scan_chain_data(input logic [31:0] data);
        ScanChainFormat format;
        format.ps_id      = data[9:4];
        format.pd_id      = data[15:10];
        format.pli_id     = data[21:16];
        format.plo_id     = data[27:22];
        format.route_mode = PERouterMode'(data[29:28]);
        format.enable     = data[30];
        return format;
    endfunction

    typedef enum logic [1:0] {
        TRACE_PID_PE_ROUTER = 2'd0,
        TRACE_PID_PE        = 2'd1,
        TRACE_PID_MBUS      = 2'd2,
        TRACE_PID_NOC_ROUTER= 2'd3
    } TRACE_PID;

    function automatic fp16_t fp16_mul(input fp16_t a, input fp16_t b);
        logic        sign_a;
        logic        sign_b;
        logic [4:0]  exp_a;
        logic [4:0]  exp_b;
        logic [9:0]  mant_a;
        logic [9:0]  mant_b;

        logic        a_is_nan;
        logic        b_is_nan;
        logic        a_is_inf;
        logic        b_is_inf;
        logic        a_is_zero;
        logic        b_is_zero;

        logic [10:0] sig_a;
        logic [10:0] sig_b;
        logic [21:0] prod;
        int          exp_res;

        logic [63:0] ext;
        logic [10:0] sig_main;
        logic        guard;
        logic        round_bit;
        logic        sticky;
        logic        inc;
        logic        sign_res;

        int shift_count;

        sign_a = a[15];
        sign_b = b[15];
        exp_a  = a[14:10];
        exp_b  = b[14:10];
        mant_a = a[9:0];
        mant_b = b[9:0];

        a_is_nan = (exp_a == 5'h1F) && (mant_a != 10'h000);
        b_is_nan = (exp_b == 5'h1F) && (mant_b != 10'h000);
        if (a_is_nan) return a | 16'h0200;
        if (b_is_nan) return b | 16'h0200;

        a_is_inf  = (exp_a == 5'h1F) && (mant_a == 10'h000);
        b_is_inf  = (exp_b == 5'h1F) && (mant_b == 10'h000);
        a_is_zero = (exp_a == 5'h00) && (mant_a == 10'h000);
        b_is_zero = (exp_b == 5'h00) && (mant_b == 10'h000);

        if ((a_is_inf && b_is_zero) || (b_is_inf && a_is_zero)) return 16'h7E00;
        if (a_is_inf || b_is_inf) begin
            sign_res = sign_a ^ sign_b;
            return {sign_res, 5'h1F, 10'h000};
        end
        if (a_is_zero || b_is_zero) begin
            sign_res = sign_a ^ sign_b;
            return {sign_res, 15'h0000};
        end

        if ((exp_a == 5'h00) && (mant_a != 10'h000)) begin
            shift_count = 0;
            while ((mant_a[9] == 1'b0) && (shift_count < 10)) begin
                mant_a = mant_a << 1;
                shift_count++;
            end
            exp_a = 5'd1;
            mant_a = mant_a & 10'h3FF;
        end

        if ((exp_b == 5'h00) && (mant_b != 10'h000)) begin
            shift_count = 0;
            while ((mant_b[9] == 1'b0) && (shift_count < 10)) begin
                mant_b = mant_b << 1;
                shift_count++;
            end
            exp_b = 5'd1;
            mant_b = mant_b & 10'h3FF;
        end

        sig_a = {1'b1, mant_a};
        sig_b = {1'b1, mant_b};

        prod = sig_a * sig_b;
        exp_res = int'(exp_a) + int'(exp_b) - 15;

        if (prod[21]) begin
            prod = prod >> 1;
            exp_res = exp_res + 1;
        end

        ext = {42'h0, prod} << 3;
        sig_main  = ext[23:13];
        guard     = ext[12];
        round_bit = ext[11];
        sticky    = |ext[10:0];

        inc = 1'b0;
        if (guard && (round_bit || sticky || sig_main[0])) begin
            inc = 1'b1;
        end

        if (inc) begin
            sig_main = sig_main + 11'd1;
            if (sig_main == 11'h400) begin
                sig_main = sig_main >> 1;
                exp_res = exp_res + 1;
            end
        end

        sign_res = sign_a ^ sign_b;

        if (exp_res >= 31) return {sign_res, 5'h1F, 10'h000};
        if (exp_res <= 0)  return {sign_res, 15'h0000};

        return {sign_res, exp_res[4:0], sig_main[9:0]};
    endfunction

    function automatic fp16_t fp16_add(input fp16_t a, input fp16_t b);
        logic        sign_a;
        logic        sign_b;
        logic [4:0]  exp_a;
        logic [4:0]  exp_b;
        logic [9:0]  mant_a;
        logic [9:0]  mant_b;

        logic        a_is_nan;
        logic        b_is_nan;
        logic        a_is_inf;
        logic        b_is_inf;
        logic        a_is_zero;
        logic        b_is_zero;

        logic [13:0] sig_a;
        logic [13:0] sig_b;
        int          exp_res;
        int          diff;
        logic [14:0] sig_res;
        logic        sign_res;

        logic [10:0] sig_main;
        logic        guard;
        logic        round_bit;
        logic        sticky;
        logic        increment;

        logic [13:0] shifted_out_mask;
        logic [13:0] shifted_out;
        logic        sticky_align;

        sign_a = a[15];
        sign_b = b[15];
        exp_a  = a[14:10];
        exp_b  = b[14:10];
        mant_a = a[9:0];
        mant_b = b[9:0];

        a_is_nan = (exp_a == 5'h1F) && (mant_a != 10'h000);
        b_is_nan = (exp_b == 5'h1F) && (mant_b != 10'h000);
        if (a_is_nan) return a;
        if (b_is_nan) return b;

        a_is_inf = (exp_a == 5'h1F) && (mant_a == 10'h000);
        b_is_inf = (exp_b == 5'h1F) && (mant_b == 10'h000);
        if (a_is_inf && b_is_inf) begin
            if (sign_a != sign_b) return 16'hFE00;
            return a;
        end
        if (a_is_inf) return a;
        if (b_is_inf) return b;

        a_is_zero = (exp_a == 5'h00) && (mant_a == 10'h000);
        b_is_zero = (exp_b == 5'h00) && (mant_b == 10'h000);
        if (a_is_zero && b_is_zero) begin
            return {(sign_a & sign_b), 15'h0000};
        end
        if (a_is_zero) return b;
        if (b_is_zero) return a;

        if (exp_a == 5'h00) return b;
        if (exp_b == 5'h00) return a;

        sig_a = {1'b1, mant_a, 3'b000};
        sig_b = {1'b1, mant_b, 3'b000};

        exp_res = exp_a;
        diff = int'(exp_a) - int'(exp_b);

        if (diff > 0) begin
            exp_res = exp_a;
            if (diff > 25) begin
                sig_b = 14'd1;
            end else begin
                shifted_out_mask = (14'(1) << diff) - 14'd1;
                shifted_out = sig_b & shifted_out_mask;
                sticky_align = (shifted_out != 14'd0);
                sig_b = sig_b >> diff;
                sig_b[0] = sig_b[0] | sticky_align;
            end
        end else if (diff < 0) begin
            diff = -diff;
            exp_res = exp_b;
            if (diff > 25) begin
                sig_a = 14'd1;
            end else begin
                shifted_out_mask = (14'(1) << diff) - 14'd1;
                shifted_out = sig_a & shifted_out_mask;
                sticky_align = (shifted_out != 14'd0);
                sig_a = sig_a >> diff;
                sig_a[0] = sig_a[0] | sticky_align;
            end
        end

        if (sign_a == sign_b) begin
            sig_res = {1'b0, sig_a} + {1'b0, sig_b};
            sign_res = sign_a;
            if (sig_res[14]) begin
                sig_res = sig_res >> 1;
                exp_res = exp_res + 1;
                if (exp_res >= 31) begin
                    return {sign_res, 5'h1F, 10'h000};
                end
            end
        end else begin
            if ((sig_a > sig_b) || ((sig_a == sig_b) && (exp_a >= exp_b))) begin
                sig_res = 15'(sig_a) - 15'(sig_b);
                sign_res = sign_a;
            end else begin
                sig_res = 15'(sig_b) - 15'(sig_a);
                sign_res = sign_b;
            end

            if (sig_res == 15'd0) begin
                return 16'h0000;
            end

            while ((sig_res[13:3] != 11'h400) && (exp_res > 0) && (sig_res != 0)) begin
                sig_res = sig_res << 1;
                exp_res = exp_res - 1;
            end

            if (exp_res <= 0) begin
                return {sign_res, 15'h0000};
            end
        end

        sig_main  = sig_res[13:3];
        guard     = sig_res[2];
        round_bit = sig_res[1];
        sticky    = sig_res[0];

        increment = 1'b0;
        if (guard && (round_bit || sticky || sig_main[0])) begin
            increment = 1'b1;
        end

        if (increment) begin
            sig_main = sig_main + 11'd1;
            if (sig_main == 11'h800) begin
                sig_main = sig_main >> 1;
                exp_res = exp_res + 1;
                if (exp_res >= 31) begin
                    return {sign_res, 5'h1F, 10'h000};
                end
            end
        end

        if (exp_res <= 0) begin
            return {sign_res, 15'h0000};
        end

        return {sign_res, exp_res[4:0], sig_main[9:0]};
    endfunction

endpackage

`endif
