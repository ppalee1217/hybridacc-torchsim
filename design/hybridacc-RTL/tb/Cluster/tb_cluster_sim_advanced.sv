//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc Testbench
// Module Name:   tb_cluster_sim_advanced
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Workload-driven Cluster RTL testbench.
//                Replays rtl_cluster_case.cfg-generated waves against
//                ComputeCluster, using a DRAM shadow in the testbench and the
//                DUT AXI slave port as the SPM staging path.
// Dependencies:  ../tb_common.svh, Cluster source stack.
// Revision:
//   2026/04/28 - Reworked to execute generated cluster cases
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`ifndef GATE_SIM
`include "cluster_rtl_stack.svh"
`endif

module tb_cluster_sim_advanced;
    import hybridacc_utils_pkg::*;
    import cluster_pkg::*;

    localparam int DEFAULT_CLOCK_PERIOD_NS = 10;
    localparam int MAX_CASE_CFG_ENTRIES    = 4096;
    localparam int WAVE_TIMEOUT_CYCLES     = 10_000_000;
    localparam int POLL_INTERVAL_CYCLES    = 1000;
    localparam int MAX_SIM_CYCLES          = 20_000_000;

    localparam logic [31:0] CLUSTER_SPM_CFG_MAP    = 32'h0000_0000;
    localparam logic [31:0] CLUSTER_SPM_CFG_UPDATE = 32'h0000_0004;
    localparam logic [31:0] CLUSTER_HDDU_BASE      = 32'h0000_1000;
    localparam logic [31:0] CLUSTER_NOC_CMD        = 32'h0000_2000;
    localparam logic [31:0] HDDU_CTRL              = 32'h0000_0800;
    localparam logic [31:0] HDDU_STATUS            = 32'h0000_0804;
    localparam logic [31:0] HDDU_PLANE_EN          = 32'h0000_0808;
    localparam logic [31:0] HDDU_PLANE_MODE        = 32'h0000_080C;
    localparam logic [31:0] HDDU_COUNTER_TX_PKT    = 32'h0000_0828;
    localparam logic [31:0] HDDU_COUNTER_TX_BYTE   = 32'h0000_082C;
    localparam logic [31:0] HDDU_COUNTER_RX_BYTE   = 32'h0000_0830;
    localparam logic [31:0] HDDU_COUNTER_STALL     = 32'h0000_0834;
    localparam logic [31:0] HDDU_ERR_CODE          = 32'h0000_081C;
    localparam logic [31:0] HDDU_ERR_INFO0         = 32'h0000_0820;
    localparam logic [31:0] HDDU_ERR_INFO1         = 32'h0000_0824;
    localparam logic [31:0] AGU_BANK_STRIDE        = 32'h0000_0100;

    localparam int AGU_PS  = 0;
    localparam int AGU_PD  = 1;
    localparam int AGU_PLI = 2;
    localparam int AGU_PLO = 3;

    typedef enum logic [2:0] {
        TENSOR_UNKNOWN     = 3'd0,
        TENSOR_ACTIVATION  = 3'd1,
        TENSOR_WEIGHT      = 3'd2,
        TENSOR_PARTIAL_SUM = 3'd3,
        TENSOR_OUTPUT      = 3'd4
    } tensor_kind_e;

    typedef enum logic [1:0] {
        DIR_UNKNOWN     = 2'd0,
        DIR_DRAM_TO_SPM = 2'd1,
        DIR_SPM_TO_DRAM = 2'd2
    } transfer_dir_e;

    typedef struct {
        logic        enabled;
        logic [31:0] base_addr;
        logic [31:0] iter0;
        logic [31:0] stride0;
        logic [31:0] iter1;
        logic [31:0] stride1;
        logic [31:0] iter2;
        logic [31:0] stride2;
        logic [31:0] iter3;
        logic [31:0] stride3;
    } addr_desc_t;

    typedef struct {
        tensor_kind_e  tensor;
        transfer_dir_e direction;
        logic [31:0]   size_words64;
        addr_desc_t    src;
        addr_desc_t    dst;
    } transfer_cfg_t;

    typedef struct {
        logic        enable;
        logic [31:0] base_addr;
        logic [31:0] base_addr_h;
        logic [31:0] stride0;
        logic [31:0] stride1;
        logic [31:0] stride2;
        logic [31:0] stride3;
        logic [31:0] lane_cfg;
        logic [31:0] tag_base;
        logic [31:0] tag_stride0;
        logic [31:0] tag_stride1;
        logic [31:0] tag_ctrl;
        logic [31:0] mask_cfg;
        logic [31:0] iter0;
        logic [31:0] iter1;
        logic [31:0] iter2;
        logic [31:0] iter3;
        logic        ultra;
    } agu_cfg_t;

    typedef struct {
        logic [31:0] global_mask;
        logic        ultra_mode;
        agu_cfg_t    agu_ps;
        agu_cfg_t    agu_pd;
        agu_cfg_t    agu_pli;
        agu_cfg_t    agu_plo;
    } plan_cfg_t;

    typedef struct {
        int  total_elements;
        int  mismatches;
        real cosine_similarity;
        real max_diff;
        real mse;
    } verify_stats_t;

    logic clk, reset_n;
    logic power_enable_i;
    logic interrupt_o;
    logic cmd_req_valid_i;
    logic cmd_req_write_i;
    logic [31:0] cmd_req_addr_i;
    logic [31:0] cmd_req_wdata_i;
    logic [3:0]  cmd_req_wstrb_i;
    logic cmd_req_ready_o;
    logic cmd_resp_valid_o;
    logic [31:0] cmd_resp_rdata_o;
    logic cmd_resp_err_o;
    logic s_axi_awvalid_i;
    logic s_axi_awready_o;
    logic [31:0] s_axi_awaddr_i;
    logic s_axi_wvalid_i;
    logic s_axi_wready_o;
    logic [63:0] s_axi_wdata_i;
    logic [7:0]  s_axi_wstrb_i;
    logic s_axi_bvalid_o;
    logic s_axi_bready_i;
    logic [1:0] s_axi_bresp_o;
    logic s_axi_arvalid_i;
    logic s_axi_arready_o;
    logic [31:0] s_axi_araddr_i;
    logic s_axi_rvalid_o;
    logic s_axi_rready_i;
    logic [63:0] s_axi_rdata_o;
    logic [1:0] s_axi_rresp_o;
    logic hsel_i;
    logic [31:0] haddr_i;
    logic hwrite_i;
    logic [1:0] htrans_i;
    logic [2:0] hsize_i;
    logic [2:0] hburst_i;
    logic [3:0] hprot_i;
    logic hready_i;
    logic [31:0] hwdata_i;
    logic hready_o;
    logic hresp_o;
    logic [31:0] hrdata_o;

    int pass_count = 0;
    int fail_count = 0;
    int x_fail_count = 0;
    int dbg_noc_plo_issue_count;
    int dbg_bus_to_pe_plo_fire_count;
    int dbg_pe_to_bus_plo_resp_fire_count;
    int dbg_bus_to_noc_plo_resp_fire_count;
    int dbg_hddu_plo_resp_count;
    int mon_agu_issue_count [4];
    int mon_spm_req_count [4];
    int mon_spm_resp_count [4];
    int mon_noc_send_count [3];
    int mon_noc_plo_req_count;
    int mon_noc_plo_resp_count;

    string data_dir = "../../output/cluster-sim/conv_k3c4ich16och64s";
    string mode_str;
    string file_activation;
    string file_weight;
    string file_partial_sum;
    string file_output;
    string file_pe_program;
    string file_scan_chain;
    int    wave_count;
    int    plan_count;
    bit    case_ultra_mode;
    real   verify_tolerance = 0.02;
    real   verify_min_cosine = 0.99;

    int dram_activation_base;
    int dram_weight_base;
    int dram_partial_sum_base;
    int dram_output_base;
    int dram_pe_program_base;

    string cfg_keys   [MAX_CASE_CFG_ENTRIES];
    string cfg_values [MAX_CASE_CFG_ENTRIES];
    int    cfg_count = 0;

    logic [15:0] activation_fp16[];
    logic [15:0] weight_fp16[];
    logic [15:0] partial_sum_fp16[];
    logic [15:0] expected_output_fp16[];
    logic [15:0] pe_program[];
    logic [31:0] scan_chain_words[];

    logic [63:0] dram_shadow [longint unsigned];

    tb_clock_reset #(.CLK_PERIOD_NS(DEFAULT_CLOCK_PERIOD_NS)) clk_rst(
        .clk(clk),
        .reset_n(reset_n)
    );

    ComputeCluster dut (
        .clk(clk),
        .reset_n(reset_n),
        .power_enable_i(power_enable_i),
        .interrupt_o(interrupt_o),
        .cmd_req_valid_i(cmd_req_valid_i),
        .cmd_req_write_i(cmd_req_write_i),
        .cmd_req_addr_i(cmd_req_addr_i),
        .cmd_req_wdata_i(cmd_req_wdata_i),
        .cmd_req_wstrb_i(cmd_req_wstrb_i),
        .cmd_req_ready_o(cmd_req_ready_o),
        .cmd_resp_valid_o(cmd_resp_valid_o),
        .cmd_resp_rdata_o(cmd_resp_rdata_o),
        .cmd_resp_err_o(cmd_resp_err_o),
        .s_axi_awvalid_i(s_axi_awvalid_i),
        .s_axi_awready_o(s_axi_awready_o),
        .s_axi_awaddr_i(s_axi_awaddr_i),
        .s_axi_wvalid_i(s_axi_wvalid_i),
        .s_axi_wready_o(s_axi_wready_o),
        .s_axi_wdata_i(s_axi_wdata_i),
        .s_axi_wstrb_i(s_axi_wstrb_i),
        .s_axi_bvalid_o(s_axi_bvalid_o),
        .s_axi_bready_i(s_axi_bready_i),
        .s_axi_bresp_o(s_axi_bresp_o),
        .s_axi_arvalid_i(s_axi_arvalid_i),
        .s_axi_arready_o(s_axi_arready_o),
        .s_axi_araddr_i(s_axi_araddr_i),
        .s_axi_rvalid_o(s_axi_rvalid_o),
        .s_axi_rready_i(s_axi_rready_i),
        .s_axi_rdata_o(s_axi_rdata_o),
        .s_axi_rresp_o(s_axi_rresp_o),
        .hsel_i(hsel_i),
        .haddr_i(haddr_i),
        .hwrite_i(hwrite_i),
        .htrans_i(htrans_i),
        .hsize_i(hsize_i),
        .hburst_i(hburst_i),
        .hprot_i(hprot_i),
        .hready_i(hready_i),
        .hwdata_i(hwdata_i),
        .hready_o(hready_o),
        .hresp_o(hresp_o),
        .hrdata_o(hrdata_o)
    );

    function automatic int cfg_get_int(string key, int default_val = 0);
        for (int i = 0; i < cfg_count; i++) begin
            if (cfg_keys[i] == key) begin
                return cfg_values[i].atoi();
            end
        end
        return default_val;
    endfunction

    function automatic bit cfg_get_bool(string key, bit default_val = 0);
        for (int i = 0; i < cfg_count; i++) begin
            if (cfg_keys[i] == key) begin
                if ((cfg_values[i] == "True") || (cfg_values[i] == "true") || (cfg_values[i] == "1"))
                    return 1'b1;
                return 1'b0;
            end
        end
        return default_val;
    endfunction

    function automatic string cfg_get_str(string key, string default_val = "");
        for (int i = 0; i < cfg_count; i++) begin
            if (cfg_keys[i] == key)
                return cfg_values[i];
        end
        return default_val;
    endfunction

    function automatic real cfg_get_real(string key, real default_val = 0.0);
        real parsed_val;
        for (int i = 0; i < cfg_count; i++) begin
            if (cfg_keys[i] == key) begin
                if ($sscanf(cfg_values[i], "%f", parsed_val) == 1)
                    return parsed_val;
                return default_val;
            end
        end
        return default_val;
    endfunction

    task automatic load_config(input string path);
        int fd;
        string line_str;
        begin
            fd = $fopen(path, "r");
            if (fd == 0) begin
                $fatal(1, "[TB] Cannot open config: %s", path);
            end
            cfg_count = 0;
            while (!$feof(fd)) begin
                automatic string key_s, val_s;
                automatic int colon_pos = -1;
                if ($fgets(line_str, fd) == 0) begin
                    continue;
                end
                while (line_str.len() > 0 && ((line_str.getc(line_str.len()-1) == "\n") ||
                                              (line_str.getc(line_str.len()-1) == "\r")))
                    line_str = line_str.substr(0, line_str.len()-2);
                if (line_str.len() == 0)
                    continue;
                for (int i = 0; i < line_str.len(); i++) begin
                    if (line_str.getc(i) == ":") begin
                        colon_pos = i;
                        break;
                    end
                end
                if (colon_pos <= 0)
                    continue;
                key_s = line_str.substr(0, colon_pos - 1);
                val_s = line_str.substr(colon_pos + 1, line_str.len() - 1);
                `TB_ASSERT(cfg_count < MAX_CASE_CFG_ENTRIES, "Case config key-value store overflow")
                cfg_keys[cfg_count]   = key_s;
                cfg_values[cfg_count] = val_s;
                cfg_count++;
            end
            $fclose(fd);
        end
    endtask

    task automatic read_binary_bytes(input string path, output byte unsigned data[]);
        int fd, size, count;
        begin
            fd = $fopen(path, "rb");
            if (fd == 0)
                $fatal(1, "[TB] Failed to open %s", path);
            void'($fseek(fd, 0, 2));
            size = $ftell(fd);
            void'($rewind(fd));
            data = new[size];
            count = $fread(data, fd);
            $fclose(fd);
            if (count != size)
                $fatal(1, "[TB] Read error %s (%0d/%0d bytes)", path, count, size);
        end
    endtask

    task automatic read_binary_u16(input string path, output logic [15:0] data[]);
        byte unsigned raw[];
        int count;
        begin
            read_binary_bytes(path, raw);
            `TB_ASSERT((raw.size() % 2) == 0, "u16 binary size must be even")
            count = raw.size() / 2;
            data = new[count];
            for (int i = 0; i < count; i++) begin
                data[i] = {raw[(i * 2) + 1], raw[i * 2]};
            end
        end
    endtask

    task automatic read_binary_u32(input string path, output logic [31:0] data[]);
        byte unsigned raw[];
        int count;
        begin
            read_binary_bytes(path, raw);
            `TB_ASSERT((raw.size() % 4) == 0, "u32 binary size must be multiple of 4")
            count = raw.size() / 4;
            data = new[count];
            for (int i = 0; i < count; i++) begin
                data[i] = {raw[(i * 4) + 3], raw[(i * 4) + 2], raw[(i * 4) + 1], raw[i * 4]};
            end
        end
    endtask

    function automatic real fp16_to_real(input logic [15:0] bits);
        int  exponent;
        int  fraction;
        real sign_factor;
        real mantissa;
        begin
            sign_factor = bits[15] ? -1.0 : 1.0;
            exponent    = bits[14:10];
            fraction    = bits[9:0];
            if ((exponent == 0) && (fraction == 0)) begin
                fp16_to_real = 0.0;
            end else if (exponent == 0) begin
                mantissa     = fraction / 1024.0;
                fp16_to_real = sign_factor * (2.0 ** -14.0) * mantissa;
            end else if (exponent == 31) begin
                fp16_to_real = sign_factor * 65504.0;
            end else begin
                mantissa     = 1.0 + (fraction / 1024.0);
                fp16_to_real = sign_factor * (2.0 ** (exponent - 15)) * mantissa;
            end
        end
    endfunction

    function automatic logic [63:0] pack_fp16x4(
        input logic [15:0] values[],
        input int          base_idx,
        input int          lanes,
        output logic [7:0] mask_out
    );
        logic [63:0] data;
        begin
            data     = 64'h0;
            mask_out = 8'h0;
            for (int lane = 0; lane < lanes; lane++) begin
                if ((base_idx + lane) < values.size()) begin
                    data |= (64'(values[base_idx + lane]) << (lane * 16));
                    mask_out[lane] = 1'b1;
                end
            end
            pack_fp16x4 = data;
        end
    endfunction

    function automatic verify_stats_t verify_fp16_vectors(
        input logic [15:0] expected[],
        input logic [15:0] received[],
        input real         tolerance
    );
        verify_stats_t stats;
        int  total;
        real dot_product, mag_expected, mag_received;
        real diff, expected_real, received_real, total_sq_err;
        begin
            stats.total_elements    = expected.size();
            stats.mismatches        = 0;
            stats.cosine_similarity = 0.0;
            stats.max_diff          = 0.0;
            stats.mse               = 0.0;

            if ((expected.size() != received.size()) || (expected.size() == 0)) begin
                stats.mismatches = (expected.size() > received.size()) ? expected.size() : received.size();
                stats.cosine_similarity = 0.0;
                stats.max_diff = 1.0e30;
                stats.mse      = 1.0e30;
                return stats;
            end

            dot_product  = 0.0;
            mag_expected = 0.0;
            mag_received = 0.0;
            total_sq_err = 0.0;
            total = expected.size();

            for (int i = 0; i < total; i++) begin
                expected_real = fp16_to_real(expected[i]);
                received_real = fp16_to_real(received[i]);
                dot_product  += expected_real * received_real;
                mag_expected += expected_real * expected_real;
                mag_received += received_real * received_real;
                diff = expected_real - received_real;
                if (diff < 0.0)
                    diff = -diff;
                total_sq_err += diff * diff;
                if (diff > stats.max_diff)
                    stats.max_diff = diff;
                if (diff > tolerance) begin
                    stats.mismatches++;
                    $display("[Mismatch] idx=%0d exp=%f (0x%04h) got=%f (0x%04h) diff=%e",
                             i, expected_real, expected[i], received_real, received[i], diff);
                end
            end

            mag_expected = $sqrt(mag_expected);
            mag_received = $sqrt(mag_received);
            stats.mse    = total_sq_err / total;

            if ((mag_expected > 0.0) && (mag_received > 0.0))
                stats.cosine_similarity = dot_product / (mag_expected * mag_received);
            else
                stats.cosine_similarity = 0.0;

            stats.total_elements = total;
            verify_fp16_vectors  = stats;
        end
    endfunction

    function automatic tensor_kind_e tensor_from_str(input string tensor_str);
        begin
            if (tensor_str == "activation")
                return TENSOR_ACTIVATION;
            if (tensor_str == "weight")
                return TENSOR_WEIGHT;
            if (tensor_str == "partial_sum")
                return TENSOR_PARTIAL_SUM;
            if (tensor_str == "output")
                return TENSOR_OUTPUT;
            return TENSOR_UNKNOWN;
        end
    endfunction

    function automatic string tensor_name(input tensor_kind_e tensor_kind);
        begin
            case (tensor_kind)
                TENSOR_ACTIVATION:  return "activation";
                TENSOR_WEIGHT:      return "weight";
                TENSOR_PARTIAL_SUM: return "partial_sum";
                TENSOR_OUTPUT:      return "output";
                default:            return "unknown";
            endcase
        end
    endfunction

    function automatic string direction_name(input transfer_dir_e direction_kind);
        begin
            case (direction_kind)
                DIR_DRAM_TO_SPM: return "dram_to_spm";
                DIR_SPM_TO_DRAM: return "spm_to_dram";
                default:         return "unknown";
            endcase
        end
    endfunction

    function automatic transfer_dir_e direction_from_str(input string direction_str);
        begin
            if (direction_str == "dram_to_spm")
                return DIR_DRAM_TO_SPM;
            if (direction_str == "spm_to_dram")
                return DIR_SPM_TO_DRAM;
            return DIR_UNKNOWN;
        end
    endfunction

    function automatic addr_desc_t get_addr_desc(input string prefix);
        addr_desc_t desc;
        begin
            desc.enabled  = cfg_get_bool({prefix, ".enabled"}, 0);
            desc.base_addr= cfg_get_int({prefix, ".base_addr"}, 0);
            desc.iter0    = cfg_get_int({prefix, ".iter0"}, 0);
            desc.stride0  = cfg_get_int({prefix, ".stride0"}, 0);
            desc.iter1    = cfg_get_int({prefix, ".iter1"}, 0);
            desc.stride1  = cfg_get_int({prefix, ".stride1"}, 0);
            desc.iter2    = cfg_get_int({prefix, ".iter2"}, 0);
            desc.stride2  = cfg_get_int({prefix, ".stride2"}, 0);
            desc.iter3    = cfg_get_int({prefix, ".iter3"}, 0);
            desc.stride3  = cfg_get_int({prefix, ".stride3"}, 0);
            return desc;
        end
    endfunction

    function automatic transfer_cfg_t get_transfer_cfg(input int wave_idx, input int transfer_idx);
        transfer_cfg_t transfer;
        string prefix;
        begin
            prefix = $sformatf("wave%0d.transfer%0d", wave_idx, transfer_idx);
            transfer.tensor       = tensor_from_str(cfg_get_str({prefix, ".tensor"}, ""));
            transfer.direction    = direction_from_str(cfg_get_str({prefix, ".direction"}, ""));
            transfer.size_words64 = cfg_get_int({prefix, ".size_words64"}, 0);
            transfer.src          = get_addr_desc({prefix, ".src"});
            transfer.dst          = get_addr_desc({prefix, ".dst"});
            return transfer;
        end
    endfunction

    function automatic agu_cfg_t get_agu_cfg(input string prefix);
        agu_cfg_t cfg;
        begin
            cfg.enable      = cfg_get_bool({prefix, ".enable"}, 0);
            cfg.base_addr   = cfg_get_int({prefix, ".base_addr"}, 0);
            cfg.base_addr_h = cfg_get_int({prefix, ".base_addr_h"}, 0);
            cfg.stride0     = cfg_get_int({prefix, ".stride0"}, 0);
            cfg.stride1     = cfg_get_int({prefix, ".stride1"}, 0);
            cfg.stride2     = cfg_get_int({prefix, ".stride2"}, 0);
            cfg.stride3     = cfg_get_int({prefix, ".stride3"}, 0);
            cfg.lane_cfg    = cfg_get_int({prefix, ".lane_cfg"}, 0);
            cfg.tag_base    = cfg_get_int({prefix, ".tag_base"}, 0);
            cfg.tag_stride0 = cfg_get_int({prefix, ".tag_stride0"}, 0);
            cfg.tag_stride1 = cfg_get_int({prefix, ".tag_stride1"}, 0);
            cfg.tag_ctrl    = cfg_get_int({prefix, ".tag_ctrl"}, 0);
            cfg.mask_cfg    = cfg_get_int({prefix, ".mask_cfg"}, 0);
            cfg.iter0       = cfg_get_int({prefix, ".iter0"}, 0);
            cfg.iter1       = cfg_get_int({prefix, ".iter1"}, 0);
            cfg.iter2       = cfg_get_int({prefix, ".iter2"}, 0);
            cfg.iter3       = cfg_get_int({prefix, ".iter3"}, 0);
            cfg.ultra       = cfg_get_bool({prefix, ".ultra"}, 0);
            return cfg;
        end
    endfunction

    function automatic plan_cfg_t get_plan_cfg(input int plan_idx);
        plan_cfg_t plan;
        string prefix;
        begin
            prefix = $sformatf("plan%0d", plan_idx);
            plan.global_mask = cfg_get_int({prefix, ".global_mask"}, 32'hF);
            plan.ultra_mode  = cfg_get_bool({prefix, ".ultra_mode"}, case_ultra_mode);
            plan.agu_ps      = get_agu_cfg({prefix, ".agu_ps"});
            plan.agu_pd      = get_agu_cfg({prefix, ".agu_pd"});
            plan.agu_pli     = get_agu_cfg({prefix, ".agu_pli"});
            plan.agu_plo     = get_agu_cfg({prefix, ".agu_plo"});
            return plan;
        end
    endfunction

    function automatic int addr_desc_word_count(input addr_desc_t desc);
        begin
            if (!desc.enabled)
                return 0;
            return desc.iter0 * desc.iter1 * desc.iter2 * desc.iter3;
        end
    endfunction

    task automatic expand_addr_desc(input addr_desc_t desc, output int unsigned addrs[]);
        int count;
        int iter0, iter1, iter2, iter3;
        int unsigned base_addr;
        begin
            count = addr_desc_word_count(desc);
            addrs = new[count];
            iter0 = desc.iter0;
            iter1 = desc.iter1;
            iter2 = desc.iter2;
            iter3 = desc.iter3;
            base_addr = desc.base_addr;
            count = 0;
            for (int idx0 = 0; idx0 < iter0; idx0++) begin
                for (int idx1 = 0; idx1 < iter1; idx1++) begin
                    for (int idx2 = 0; idx2 < iter2; idx2++) begin
                        for (int idx3 = 0; idx3 < iter3; idx3++) begin
                            addrs[count] = base_addr
                                        + (idx0 * desc.stride0)
                                        + (idx1 * desc.stride1)
                                        + (idx2 * desc.stride2)
                                        + (idx3 * desc.stride3);
                            count++;
                        end
                    end
                end
            end
        end
    endtask

    function automatic logic [63:0] dram_read64_shadow(input int unsigned byte_addr);
        longint unsigned word_idx;
        begin
            `TB_ASSERT((byte_addr[2:0] == 3'b000), "DRAM shadow reads must be 64-bit aligned")
            word_idx = byte_addr >> 3;
            if (dram_shadow.exists(word_idx))
                return dram_shadow[word_idx];
            return 64'h0;
        end
    endfunction

    task automatic dram_write64_shadow(input int unsigned byte_addr, input logic [63:0] data64);
        longint unsigned word_idx;
        begin
            `TB_ASSERT((byte_addr[2:0] == 3'b000), "DRAM shadow writes must be 64-bit aligned")
            word_idx = byte_addr >> 3;
            dram_shadow[word_idx] = data64;
        end
    endtask

    task automatic preload_fp16_into_dram(input int unsigned base_addr, input logic [15:0] values[]);
        logic [63:0] packed_word;
        logic [7:0] packed_mask;
        begin
            for (int i = 0; i < values.size(); i += 4) begin
                packed_word = pack_fp16x4(values, i, 4, packed_mask);
                dram_write64_shadow(base_addr + ((i / 4) * 8), packed_word);
            end
        end
    endtask

    task automatic collect_output_fp16(input int unsigned base_addr, input int elem_count, output logic [15:0] values[]);
        logic [63:0] data64;
        begin
            values = new[elem_count];
            for (int word_idx = 0; word_idx < ((elem_count + 3) / 4); word_idx++) begin
                data64 = dram_read64_shadow(base_addr + (word_idx * 8));
                for (int lane = 0; lane < 4; lane++) begin
                    int elem_idx;
                    elem_idx = (word_idx * 4) + lane;
                    if (elem_idx < elem_count)
                        values[elem_idx] = data64[(lane * 16) +: 16];
                end
            end
        end
    endtask

    task automatic ahb_write(input int unsigned addr, input logic [31:0] data);
        begin
            @(negedge clk);
            hsel_i   = 1'b1;
            haddr_i  = addr;
            hwrite_i = 1'b1;
            htrans_i = 2'b10;
            hwdata_i = data;
            @(posedge clk);
            @(negedge clk);
            hsel_i   = 1'b0;
            hwrite_i = 1'b0;
            htrans_i = 2'b00;
            hwdata_i = 32'h0;
        end
    endtask

    task automatic ahb_read(input int unsigned addr, output logic [31:0] data);
        begin
            @(negedge clk);
            hsel_i   = 1'b1;
            haddr_i  = addr;
            hwrite_i = 1'b0;
            htrans_i = 2'b10;
            #(`TB_SETTLE);
            data = hrdata_o;
            @(posedge clk);
            @(negedge clk);
            hsel_i   = 1'b0;
            htrans_i = 2'b00;
        end
    endtask

    task automatic dma_write64(input int unsigned byte_addr, input logic [63:0] data64);
        begin
            @(negedge clk);
            s_axi_awaddr_i  = byte_addr;
            s_axi_awvalid_i = 1'b1;
            while (!s_axi_awready_o) @(posedge clk);
            @(posedge clk);
            s_axi_awvalid_i = 1'b0;

            @(negedge clk);
            s_axi_wdata_i  = data64;
            s_axi_wstrb_i  = 8'hFF;
            s_axi_wvalid_i = 1'b1;
            while (!s_axi_wready_o) @(posedge clk);
            @(posedge clk);
            s_axi_wvalid_i = 1'b0;
            s_axi_bready_i = 1'b1;
            while (!s_axi_bvalid_o) @(posedge clk);
            @(posedge clk);
            s_axi_bready_i = 1'b0;
        end
    endtask

    task automatic dma_read64(input int unsigned byte_addr, output logic [63:0] data64);
        begin
            @(negedge clk);
            s_axi_araddr_i  = byte_addr;
            s_axi_arvalid_i = 1'b1;
            while (!s_axi_arready_o) @(posedge clk);
            @(posedge clk);
            s_axi_arvalid_i = 1'b0;
            s_axi_rready_i = 1'b1;
            while (!s_axi_rvalid_o) @(posedge clk);
            #(`TB_SETTLE);
            data64 = s_axi_rdata_o;
            @(posedge clk);
            s_axi_rready_i = 1'b0;
        end
    endtask

    function automatic logic [31:0] pack_noc_cmd(input logic [3:0] cmd, input logic [31:0] param);
        begin
            return (param & 32'hFFFF_FFF0) | {28'h0, cmd[3:0]};
        end
    endfunction

    function automatic logic [31:0] pack_load_program(input logic [15:0] im_addr_bytes, input logic [15:0] inst16);
        logic [31:0] payload;
        begin
            payload = 32'h0;
            payload |= ((im_addr_bytes & PE_ROUTER_IM_ADDR_MASK) << PE_ROUTER_IM_ADDR_OFFSET);
            payload |= ((inst16 & PE_ROUTER_IM_DATA_MASK) << PE_ROUTER_IM_DATA_OFFSET);
            return pack_noc_cmd(CMD_LOAD_PROGRAM, payload);
        end
    endfunction

    task automatic noc_cmd_write(input logic [31:0] cmd_word);
        begin
            ahb_write(CLUSTER_NOC_CMD, cmd_word);
        end
    endtask

    task automatic config_spm_map(input logic [7:0] map_val);
        begin
            ahb_write(CLUSTER_SPM_CFG_MAP, {24'h0, map_val});
            ahb_write(CLUSTER_SPM_CFG_UPDATE, 32'h1);
        end
    endtask

    task automatic configure_scan_chain;
        begin
            if (scan_chain_words.size() == 0)
                return;
            $display("[TB] Loading scan chain (%0d words)", scan_chain_words.size());
            for (int i = scan_chain_words.size() - 1; i >= 0; i--) begin
                noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, scan_chain_words[i]));
            end
            repeat (5) @(posedge clk);
            $display("[TB][DBG] scan act port0.pe0  en=%0b mode=%0d", dut.noc.router_enable_sig[0][0],  dut.noc.router_mode_sig[0][0]);
            $display("[TB][DBG] scan act port0.pe15 en=%0b mode=%0d", dut.noc.router_enable_sig[0][15], dut.noc.router_mode_sig[0][15]);
            $display("[TB][DBG] scan act port1.pe0  en=%0b mode=%0d", dut.noc.router_enable_sig[1][0],  dut.noc.router_mode_sig[1][0]);
            $display("[TB][DBG] scan act port1.pe15 en=%0b mode=%0d", dut.noc.router_enable_sig[1][15], dut.noc.router_mode_sig[1][15]);
            $display("[TB][DBG] scan act port2.pe0  en=%0b mode=%0d", dut.noc.router_enable_sig[2][0],  dut.noc.router_mode_sig[2][0]);
            $display("[TB][DBG] scan act port2.pe15 en=%0b mode=%0d", dut.noc.router_enable_sig[2][15], dut.noc.router_mode_sig[2][15]);
        end
    endtask

    task automatic load_pe_program_into_cluster;
        begin
            `TB_ASSERT(pe_program.size() > 0, "PE program must not be empty")
            $display("[TB] Loading PE program (%0d instructions)", pe_program.size());
            for (int pc = 0; pc < pe_program.size(); pc++) begin
                noc_cmd_write(pack_load_program(pc * 2, pe_program[pc]));
            end
            repeat (5) @(posedge clk);
            $display("[TB][DBG] imem port0.pe0[0]=0x%04h port1.pe0[0]=0x%04h port2.pe0[0]=0x%04h",
                     dut.noc.gen_ports[0].gen_pe[0].pe.if_id_stage.IM.mem[0],
                     dut.noc.gen_ports[1].gen_pe[0].pe.if_id_stage.IM.mem[0],
                     dut.noc.gen_ports[2].gen_pe[0].pe.if_id_stage.IM.mem[0]);
            $display("[TB][DBG] imem port0.pe0[1]=0x%04h port1.pe0[1]=0x%04h port2.pe0[1]=0x%04h",
                     dut.noc.gen_ports[0].gen_pe[0].pe.if_id_stage.IM.mem[1],
                     dut.noc.gen_ports[1].gen_pe[0].pe.if_id_stage.IM.mem[1],
                     dut.noc.gen_ports[2].gen_pe[0].pe.if_id_stage.IM.mem[1]);
        end
    endtask

    task automatic cfg_agu(input int bank, input agu_cfg_t cfg);
        int unsigned base_addr;
        begin
            if (!cfg.enable)
                return;
            base_addr = CLUSTER_HDDU_BASE + (bank * AGU_BANK_STRIDE);
            ahb_write(base_addr + AGU_REG_BASE_ADDR,   cfg.base_addr);
            ahb_write(base_addr + AGU_REG_BASE_ADDR_H, cfg.base_addr_h);
            ahb_write(base_addr + AGU_REG_ITER01,      {cfg.iter1[15:0], cfg.iter0[15:0]});
            ahb_write(base_addr + AGU_REG_ITER23,      {cfg.iter3[15:0], cfg.iter2[15:0]});
            ahb_write(base_addr + AGU_REG_STRIDE0,     cfg.stride0);
            ahb_write(base_addr + AGU_REG_STRIDE1,     cfg.stride1);
            ahb_write(base_addr + AGU_REG_STRIDE2,     cfg.stride2);
            ahb_write(base_addr + AGU_REG_STRIDE3,     cfg.stride3);
            ahb_write(base_addr + AGU_REG_LANE_CFG,    cfg.lane_cfg);
            ahb_write(base_addr + AGU_REG_TAG_BASE,    cfg.tag_base);
            ahb_write(base_addr + AGU_REG_TAG_STRIDE0, cfg.tag_stride0);
            ahb_write(base_addr + AGU_REG_TAG_STRIDE1, cfg.tag_stride1);
            ahb_write(base_addr + AGU_REG_TAG_CTRL,    cfg.tag_ctrl);
            ahb_write(base_addr + AGU_REG_MASK_CFG,    cfg.mask_cfg);
            ahb_write(base_addr + AGU_REG_CTRL,        cfg.ultra ? (32'h1 << AGU_CTRL_ULTRA_BIT) : 32'h0);
        end
    endtask

    task automatic cfg_hddu_global(input logic [31:0] plane_en, input logic [31:0] plane_mode);
        begin
            ahb_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN, plane_en);
            ahb_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, plane_mode);
        end
    endtask

    task automatic start_hddu_only;
        begin
            ahb_write(CLUSTER_HDDU_BASE + HDDU_CTRL, 32'h1 << CTRL_START);
        end
    endtask

    task automatic stop_hddu_only;
        begin
            ahb_write(CLUSTER_HDDU_BASE + HDDU_CTRL, 32'h1 << CTRL_STOP);
        end
    endtask

    task automatic start_pe_only;
        begin
            ahb_write(CLUSTER_MMIO_BASE + CLUSTER_REG_CTRL, 32'h1 << CTRL_START);
        end
    endtask

    task automatic stop_pe_only;
        begin
            ahb_write(CLUSTER_MMIO_BASE + CLUSTER_REG_CTRL, 32'h1 << CTRL_STOP);
        end
    endtask

    task automatic dump_hddu_error_info;
        logic [31:0] err_code;
        logic [31:0] err_info0;
        logic [31:0] err_info1;
        begin
            ahb_read(CLUSTER_HDDU_BASE + HDDU_ERR_CODE, err_code);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_ERR_INFO0, err_info0);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_ERR_INFO1, err_info1);
            $display("[TB][ERR] HDDU err_code=0x%08h err_info0=0x%08h err_info1=0x%08h",
                     err_code, err_info0, err_info1);
        end
    endtask

    task automatic wait_hddu_done(output bit success);
        logic [31:0] status_word;
        int elapsed_cycles;
        int poll_count;
        begin
            success = 1'b0;
            elapsed_cycles = 0;
            poll_count = 0;
            while (elapsed_cycles < WAVE_TIMEOUT_CYCLES) begin
                ahb_read(CLUSTER_HDDU_BASE + HDDU_STATUS, status_word);
                if ((poll_count < 8) || ((poll_count % 100) == 0)) begin
                    $display("[TB][DBG] wait poll=%0d time=%0t status=0x%08h irq=%0b agu_busy=%0b%0b%0b%0b agu_done=%0b%0b%0b%0b fsm=%0d/%0d/%0d/%0d sw=%0b%0b%0b sh=%0b%0b%0b recv=%0b tx_pkt=%0d rx_byte=%0d pe_busy=%0b%0b%0b",
                             poll_count,
                             $time,
                             status_word,
                             interrupt_o,
                             dut.hddu.agu_busy_sig[3], dut.hddu.agu_busy_sig[2], dut.hddu.agu_busy_sig[1], dut.hddu.agu_busy_sig[0],
                             dut.hddu.agu_done_sig[3], dut.hddu.agu_done_sig[2], dut.hddu.agu_done_sig[1], dut.hddu.agu_done_sig[0],
                             dut.hddu.agu_fsm_state_sig[0], dut.hddu.agu_fsm_state_sig[1], dut.hddu.agu_fsm_state_sig[2], dut.hddu.agu_fsm_state_sig[3],
                             dut.hddu.send_wait_valid_reg[2], dut.hddu.send_wait_valid_reg[1], dut.hddu.send_wait_valid_reg[0],
                             dut.hddu.send_hold_valid_reg[2], dut.hddu.send_hold_valid_reg[1], dut.hddu.send_hold_valid_reg[0],
                             dut.hddu.recv_addr_pending_reg,
                             dut.hddu.counter_tx_pkt_reg,
                             dut.hddu.counter_rx_byte_reg,
                             dut.noc.pe_busy_sig[2][0], dut.noc.pe_busy_sig[1][0], dut.noc.pe_busy_sig[0][0]);
                    $display("[TB][DBG] lane0 pc=%0d/%0d/%0d exe_a=%0d/%0d/%0d pd_full=%0b/%0b/%0b pli_empty=%0b/%0b/%0b plo_empty=%0b/%0b/%0b ln01=%0b/%0b ln12=%0b/%0b",
                             dut.noc.gen_ports[0].gen_pe[0].pe.if_id_pc_sig,
                             dut.noc.gen_ports[1].gen_pe[0].pe.if_id_pc_sig,
                             dut.noc.gen_ports[2].gen_pe[0].pe.if_id_pc_sig,
                             dut.noc.gen_ports[0].gen_pe[0].pe.exe_a_stage.state_reg,
                             dut.noc.gen_ports[1].gen_pe[0].pe.exe_a_stage.state_reg,
                             dut.noc.gen_ports[2].gen_pe[0].pe.exe_a_stage.state_reg,
                             dut.noc.gen_ports[0].gen_pe[0].pe.router.pd_full,
                             dut.noc.gen_ports[1].gen_pe[0].pe.router.pd_full,
                             dut.noc.gen_ports[2].gen_pe[0].pe.router.pd_full,
                             dut.noc.gen_ports[0].gen_pe[0].pe.router.pli_empty,
                             dut.noc.gen_ports[1].gen_pe[0].pe.router.pli_empty,
                             dut.noc.gen_ports[2].gen_pe[0].pe.router.pli_empty,
                             dut.noc.gen_ports[0].gen_pe[0].pe.router.plo_empty,
                             dut.noc.gen_ports[1].gen_pe[0].pe.router.plo_empty,
                             dut.noc.gen_ports[2].gen_pe[0].pe.router.plo_empty,
                             dut.noc.ln_valid[1][0], dut.noc.ln_ready[1][0],
                             dut.noc.ln_valid[2][0], dut.noc.ln_ready[2][0]);
                    $display("[TB][DBG] plo path tag=%0d rxmask2=0x%04h pe_vr=%0b/%0b bus_vr=%0b/%0b noc_pending=%0b router_plo_empty=%0b p2req=%0b/%0b pe_req_ready=%0b",
                             dut.hddu.noc_plo_out_addr[5:0],
                             dut.noc.gen_ports[2].mbus.rx_mask_reg,
                             dut.noc.pe_to_bus_plo_valid[2][0],
                             dut.noc.pe_to_bus_plo_ready[2][0],
                             dut.noc.bus_to_noc_resp_valid[2],
                             dut.noc.bus_to_noc_resp_ready[2],
                             dut.noc.router.pending_read_reg,
                             dut.noc.router.plo_fifo_empty,
                             dut.noc.noc_plo_to_bus_valid[2],
                             dut.noc.noc_plo_to_bus_ready[2],
                             dut.noc.gen_ports[2].gen_pe[0].pe.router.noc_plo_req_ready);
                end
                if (status_word[STATUS_ERROR]) begin
                    $error("[TB] HDDU entered error state status=0x%08h", status_word);
                    dump_hddu_error_info();
                    return;
                end
                if (status_word[STATUS_DONE] || interrupt_o) begin
                    success = 1'b1;
                    return;
                end
                repeat (POLL_INTERVAL_CYCLES) @(posedge clk);
                elapsed_cycles += POLL_INTERVAL_CYCLES;
                poll_count++;
            end
            $error("[TB] HDDU timeout after %0d cycles", WAVE_TIMEOUT_CYCLES);
        end
    endtask

    task automatic execute_transfer(input transfer_cfg_t transfer);
        int unsigned src_addrs[];
        int unsigned dst_addrs[];
        logic [63:0] data64;
        int count;
        begin
            `TB_ASSERT(transfer.tensor != TENSOR_UNKNOWN, "Unknown tensor in transfer config")
            `TB_ASSERT(transfer.direction != DIR_UNKNOWN, "Unknown transfer direction in transfer config")

            expand_addr_desc(transfer.src, src_addrs);
            expand_addr_desc(transfer.dst, dst_addrs);
            count = transfer.size_words64;

            `TB_ASSERT(src_addrs.size() == count, "Source descriptor count mismatch")
            `TB_ASSERT(dst_addrs.size() == count, "Destination descriptor count mismatch")

            for (int i = 0; i < count; i++) begin
                `TB_ASSERT((src_addrs[i][2:0] == 3'b000), "Source transfer address must be 64-bit aligned")
                `TB_ASSERT((dst_addrs[i][2:0] == 3'b000), "Destination transfer address must be 64-bit aligned")
                case (transfer.direction)
                    DIR_DRAM_TO_SPM: begin
                        data64 = dram_read64_shadow(src_addrs[i]);
                        dma_write64(dst_addrs[i], data64);
                    end
                    DIR_SPM_TO_DRAM: begin
                        dma_read64(src_addrs[i], data64);
                        dram_write64_shadow(dst_addrs[i], data64);
                    end
                    default: begin
                        $fatal(1, "[TB] Unsupported transfer direction");
                    end
                endcase
            end
        end
    endtask

    task automatic load_case_artifacts;
        begin
            file_activation   = cfg_get_str("file_activation", "input_activation.bin");
            file_weight       = cfg_get_str("file_weight", "input_weight.bin");
            file_partial_sum  = cfg_get_str("file_partial_sum", "input_partial_sum.bin");
            file_output       = cfg_get_str("file_output", "output_partial_sum.bin");
            file_pe_program   = cfg_get_str("file_pe_program", "pe_program.bin");
            file_scan_chain   = cfg_get_str("file_scan_chain", "scan_chain.bin");
            mode_str          = cfg_get_str("mode", "conv2d");
            case_ultra_mode   = cfg_get_bool("ultra_mode", 0);
            verify_tolerance  = cfg_get_real("verify_tolerance", verify_tolerance);
            verify_min_cosine = cfg_get_real("verify_min_cosine", verify_min_cosine);
            void'($value$plusargs("VERIFY_TOL=%f", verify_tolerance));

            dram_activation_base  = cfg_get_int("dram.activation", 0);
            dram_weight_base      = cfg_get_int("dram.weight", 32'h1000_0000);
            dram_partial_sum_base = cfg_get_int("dram.partial_sum", 32'h2000_0000);
            dram_output_base      = cfg_get_int("dram.output", 32'h3000_0000);
            dram_pe_program_base  = cfg_get_int("dram.pe_program", 32'h4000_0000);
            wave_count            = cfg_get_int("wave_count", 0);
            plan_count            = cfg_get_int("plan_count", 0);

            `TB_ASSERT(wave_count > 0, "wave_count must be positive")
            `TB_ASSERT(plan_count > 0, "plan_count must be positive")

            read_binary_u16({data_dir, "/", file_activation}, activation_fp16);
            read_binary_u16({data_dir, "/", file_weight}, weight_fp16);
            read_binary_u16({data_dir, "/", file_output}, expected_output_fp16);
            read_binary_u16({data_dir, "/", file_pe_program}, pe_program);

            if (file_partial_sum.len() > 0)
                read_binary_u16({data_dir, "/", file_partial_sum}, partial_sum_fp16);
            else
                partial_sum_fp16 = new[0];

            if ((file_scan_chain.len() > 0) && (file_scan_chain != "none"))
                read_binary_u32({data_dir, "/", file_scan_chain}, scan_chain_words);
            else
                scan_chain_words = new[0];

            $display("[TB] DATA_DIR=%s", data_dir);
            $display("[TB] mode=%s ultra_mode=%0d wave_count=%0d plan_count=%0d verify_tolerance=%0.5f",
                     mode_str, case_ultra_mode, wave_count, plan_count, verify_tolerance);
            $display("[TB] verify_min_cosine=%0.5f", verify_min_cosine);
            $display("[TB] Loaded activation=%0d weight=%0d partial_sum=%0d expected=%0d program=%0d scan_chain=%0d",
                     activation_fp16.size(), weight_fp16.size(), partial_sum_fp16.size(),
                     expected_output_fp16.size(), pe_program.size(), scan_chain_words.size());

            dram_shadow.delete();
            preload_fp16_into_dram(dram_activation_base, activation_fp16);
            preload_fp16_into_dram(dram_weight_base, weight_fp16);
            if (partial_sum_fp16.size() > 0)
                preload_fp16_into_dram(dram_partial_sum_base, partial_sum_fp16);
        end
    endtask

    task automatic execute_wave(input int wave_idx, output bit success);
        plan_cfg_t     plan;
        transfer_cfg_t transfer;
        int transfer_count;
        int ps_expected;
        int pd_expected;
        int pli_expected;
        int plo_expected;
        int agu_issue_start [4];
        int spm_req_start [4];
        int spm_resp_start [4];
        int noc_send_start [3];
        int noc_plo_req_start;
        int noc_plo_resp_start;
        logic [31:0] hddu_status;
        logic [31:0] tx_pkt;
        logic [31:0] tx_byte;
        logic [31:0] rx_byte;
        logic [31:0] stall;
        logic [7:0]  spm_map;
        logic        plan_ultra;
        begin
            success = 1'b0;
            plan = get_plan_cfg(wave_idx);
            transfer_count = cfg_get_int($sformatf("wave%0d.transfer_count", wave_idx), 0);
            spm_map = cfg_get_int($sformatf("wave%0d.spm_map", wave_idx), 8'hE4);
            ps_expected = plan.agu_ps.enable ? (plan.agu_ps.iter0 * plan.agu_ps.iter1 * plan.agu_ps.iter2 * plan.agu_ps.iter3) : 0;
            pd_expected = plan.agu_pd.enable ? (plan.agu_pd.iter0 * plan.agu_pd.iter1 * plan.agu_pd.iter2 * plan.agu_pd.iter3) : 0;
            pli_expected = plan.agu_pli.enable ? (plan.agu_pli.iter0 * plan.agu_pli.iter1 * plan.agu_pli.iter2 * plan.agu_pli.iter3) : 0;
            plo_expected = plan.agu_plo.enable ? (plan.agu_plo.iter0 * plan.agu_plo.iter1 * plan.agu_plo.iter2 * plan.agu_plo.iter3) : 0;
            for (int plane = 0; plane < 4; plane++) begin
                agu_issue_start[plane] = mon_agu_issue_count[plane];
                spm_req_start[plane] = mon_spm_req_count[plane];
                spm_resp_start[plane] = mon_spm_resp_count[plane];
            end
            for (int plane = 0; plane < 3; plane++) begin
                noc_send_start[plane] = mon_noc_send_count[plane];
            end
            noc_plo_req_start = mon_noc_plo_req_count;
            noc_plo_resp_start = mon_noc_plo_resp_count;

            $display("[TB] Wave %0d/%0d", wave_idx + 1, wave_count);
            config_spm_map(spm_map);

            for (int transfer_idx = 0; transfer_idx < transfer_count; transfer_idx++) begin
                transfer = get_transfer_cfg(wave_idx, transfer_idx);
                if (transfer.direction == DIR_DRAM_TO_SPM) begin
                    $display("[TB][DBG] wave%0d transfer%0d tensor=%s dir=%s words=%0d",
                             wave_idx, transfer_idx, tensor_name(transfer.tensor),
                             direction_name(transfer.direction), transfer.size_words64);
                    execute_transfer(transfer);
                end
            end

            $display("[TB][DBG] wave%0d d2s complete", wave_idx);

            cfg_agu(AGU_PS, plan.agu_ps);
            cfg_agu(AGU_PD, plan.agu_pd);
            cfg_agu(AGU_PLI, plan.agu_pli);
            cfg_agu(AGU_PLO, plan.agu_plo);
            plan_ultra = plan.ultra_mode || case_ultra_mode;
            cfg_hddu_global(plan.global_mask, plan_ultra ? 32'h2 : 32'h1);

            $display("[TB][DBG] wave%0d start_hddu", wave_idx);
            start_hddu_only();
            repeat (4) @(posedge clk);
            $display("[TB][DBG] wave%0d pe_busy port0.pe0=%0b port1.pe0=%0b port2.pe0=%0b",
                     wave_idx,
                     dut.noc.pe_busy_sig[0][0],
                     dut.noc.pe_busy_sig[1][0],
                     dut.noc.pe_busy_sig[2][0]);
            wait_hddu_done(success);
            $display("[TB][DBG] wave%0d wait_hddu_done success=%0d", wave_idx, success);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_STATUS, hddu_status);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_TX_PKT, tx_pkt);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_TX_BYTE, tx_byte);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_RX_BYTE, rx_byte);
            ahb_read(CLUSTER_HDDU_BASE + HDDU_COUNTER_STALL, stall);
            $display("[TB][DBG] wave%0d hddu_status=0x%08h tx_pkt=%0d tx_byte=%0d rx_byte=%0d stall=%0d",
                     wave_idx, hddu_status, tx_pkt, tx_byte, rx_byte, stall);
            $display("[TB][DBG] wave%0d expected ops ps/pd/pli/plo=%0d/%0d/%0d/%0d",
                     wave_idx, ps_expected, pd_expected, pli_expected, plo_expected);
            $display("[TB][DBG] wave%0d AGU issue delta ps/pd/pli/plo=%0d/%0d/%0d/%0d",
                     wave_idx,
                     mon_agu_issue_count[AGU_PS] - agu_issue_start[AGU_PS],
                     mon_agu_issue_count[AGU_PD] - agu_issue_start[AGU_PD],
                     mon_agu_issue_count[AGU_PLI] - agu_issue_start[AGU_PLI],
                     mon_agu_issue_count[AGU_PLO] - agu_issue_start[AGU_PLO]);
            $display("[TB][DBG] wave%0d SPM req delta ps/pd/pli/plo=%0d/%0d/%0d/%0d",
                     wave_idx,
                     mon_spm_req_count[AGU_PS] - spm_req_start[AGU_PS],
                     mon_spm_req_count[AGU_PD] - spm_req_start[AGU_PD],
                     mon_spm_req_count[AGU_PLI] - spm_req_start[AGU_PLI],
                     mon_spm_req_count[AGU_PLO] - spm_req_start[AGU_PLO]);
            $display("[TB][DBG] wave%0d SPM resp delta ps/pd/pli/plo=%0d/%0d/%0d/%0d",
                     wave_idx,
                     mon_spm_resp_count[AGU_PS] - spm_resp_start[AGU_PS],
                     mon_spm_resp_count[AGU_PD] - spm_resp_start[AGU_PD],
                     mon_spm_resp_count[AGU_PLI] - spm_resp_start[AGU_PLI],
                     mon_spm_resp_count[AGU_PLO] - spm_resp_start[AGU_PLO]);
            $display("[TB][DBG] wave%0d NoC send delta ps/pd/pli=%0d/%0d/%0d plo_req/resp=%0d/%0d",
                     wave_idx,
                     mon_noc_send_count[AGU_PS] - noc_send_start[AGU_PS],
                     mon_noc_send_count[AGU_PD] - noc_send_start[AGU_PD],
                     mon_noc_send_count[AGU_PLI] - noc_send_start[AGU_PLI],
                     mon_noc_plo_req_count - noc_plo_req_start,
                     mon_noc_plo_resp_count - noc_plo_resp_start);

            stop_hddu_only();

            if (!success) begin
                return;
            end

            for (int transfer_idx = 0; transfer_idx < transfer_count; transfer_idx++) begin
                transfer = get_transfer_cfg(wave_idx, transfer_idx);
                if (transfer.direction == DIR_SPM_TO_DRAM) begin
                    $display("[TB][DBG] wave%0d transfer%0d tensor=%s dir=%s words=%0d",
                             wave_idx, transfer_idx, tensor_name(transfer.tensor),
                             direction_name(transfer.direction), transfer.size_words64);
                    execute_transfer(transfer);
                end
            end

            $display("[TB][DBG] wave%0d s2d complete", wave_idx);

            success = 1'b1;
        end
    endtask

    task automatic verify_output_region;
        logic [15:0] received_output_fp16[];
        verify_stats_t stats;
        begin
            collect_output_fp16(dram_output_base, expected_output_fp16.size(), received_output_fp16);
            stats = verify_fp16_vectors(expected_output_fp16, received_output_fp16, verify_tolerance);
            if (stats.cosine_similarity < verify_min_cosine) begin
                $error("[TB] Output mismatch: mismatches=%0d total=%0d max_diff=%e mse=%e cosine=%f (min=%f)",
                       stats.mismatches, stats.total_elements, stats.max_diff, stats.mse,
                       stats.cosine_similarity, verify_min_cosine);
                fail_count = fail_count + 1;
            end else begin
                $display("[PASS] Cluster output compare mismatches=%0d total=%0d max_diff=%e mse=%e cosine=%f",
                         stats.mismatches, stats.total_elements, stats.max_diff,
                         stats.mse, stats.cosine_similarity);
                pass_count = pass_count + 1;
            end
        end
    endtask

    always_ff @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            dbg_noc_plo_issue_count <= 0;
            dbg_bus_to_pe_plo_fire_count <= 0;
            dbg_pe_to_bus_plo_resp_fire_count <= 0;
            dbg_bus_to_noc_plo_resp_fire_count <= 0;
            dbg_hddu_plo_resp_count <= 0;
            for (int plane = 0; plane < 4; plane++) begin
                mon_agu_issue_count[plane] <= 0;
                mon_spm_req_count[plane] <= 0;
                mon_spm_resp_count[plane] <= 0;
            end
            for (int plane = 0; plane < 3; plane++) begin
                mon_noc_send_count[plane] <= 0;
            end
            mon_noc_plo_req_count <= 0;
            mon_noc_plo_resp_count <= 0;
        end else begin
            for (int plane = 0; plane < 4; plane++) begin
                if (dut.hddu.agu_gen_valid_sig[plane] && dut.hddu.agu_gen_ready_sig[plane]) begin
                    mon_agu_issue_count[plane] <= mon_agu_issue_count[plane] + 1;
                end
                if (dut.hddu.spm_req_valid[plane] && dut.hddu.spm_req_ready[plane]) begin
                    mon_spm_req_count[plane] <= mon_spm_req_count[plane] + 1;
                end
                if (dut.hddu.spm_resp_valid[plane] && dut.hddu.spm_resp_ready[plane]) begin
                    mon_spm_resp_count[plane] <= mon_spm_resp_count[plane] + 1;
                end
            end

            if (dut.hddu.noc_ps_out_valid && dut.hddu.noc_ps_out_ready) begin
                mon_noc_send_count[AGU_PS] <= mon_noc_send_count[AGU_PS] + 1;
            end
            if (dut.hddu.noc_pd_out_valid && dut.hddu.noc_pd_out_ready) begin
                mon_noc_send_count[AGU_PD] <= mon_noc_send_count[AGU_PD] + 1;
            end
            if (dut.hddu.noc_pli_out_valid && dut.hddu.noc_pli_out_ready) begin
                mon_noc_send_count[AGU_PLI] <= mon_noc_send_count[AGU_PLI] + 1;
            end
            if (dut.hddu.noc_plo_out_valid && dut.hddu.noc_plo_out_ready) begin
                mon_noc_plo_req_count <= mon_noc_plo_req_count + 1;
            end
            if (dut.hddu.noc_plo_in_valid && dut.hddu.noc_plo_in_ready) begin
                mon_noc_plo_resp_count <= mon_noc_plo_resp_count + 1;
            end

            if ((dbg_noc_plo_issue_count < 16)
                && dut.noc.noc_plo_to_bus_valid[2]
                && dut.noc.noc_plo_to_bus_ready[2]) begin
                $display("[TB][DBG_EVT] noc_plo_issue[%0d] tag=%0d router_pending=%0b router_fifo_empty=%0b",
                         dbg_noc_plo_issue_count,
                         dut.noc.noc_plo_to_bus_data[2].addr[5:0],
                         dut.noc.router.pending_read_reg,
                         dut.noc.router.plo_fifo_empty);
                dbg_noc_plo_issue_count <= dbg_noc_plo_issue_count + 1;
            end

            if ((dbg_bus_to_pe_plo_fire_count < 16)
                && dut.noc.bus_to_pe_plo_valid[2][0]
                && dut.noc.bus_to_pe_plo_ready[2][0]) begin
                $display("[TB][DBG_EVT] bus_to_pe_plo_fire[%0d] lane0 tag=%0d plo_empty=%0b",
                         dbg_bus_to_pe_plo_fire_count,
                         dut.noc.bus_to_pe_plo_data[2][0].addr[5:0],
                         dut.noc.gen_ports[2].gen_pe[0].pe.router.plo_empty);
                dbg_bus_to_pe_plo_fire_count <= dbg_bus_to_pe_plo_fire_count + 1;
            end

            if ((dbg_pe_to_bus_plo_resp_fire_count < 16)
                && dut.noc.pe_to_bus_plo_valid[2][0]
                && dut.noc.pe_to_bus_plo_ready[2][0]) begin
                $display("[TB][DBG_EVT] pe_to_bus_plo_resp_fire[%0d] lane0 status=%0d data0=0x%016h",
                         dbg_pe_to_bus_plo_resp_fire_count,
                         dut.noc.pe_to_bus_plo_data[2][0].status,
                         dut.noc.pe_to_bus_plo_data[2][0].data);
                dbg_pe_to_bus_plo_resp_fire_count <= dbg_pe_to_bus_plo_resp_fire_count + 1;
            end

            if ((dbg_bus_to_noc_plo_resp_fire_count < 16)
                && dut.noc.bus_to_noc_resp_valid[2]
                && dut.noc.bus_to_noc_resp_ready[2]) begin
                $display("[TB][DBG_EVT] bus_to_noc_plo_resp_fire[%0d] port2 status=%0d data0=0x%016h rxmask=0x%04h",
                         dbg_bus_to_noc_plo_resp_fire_count,
                         dut.noc.bus_to_noc_resp_data[2].status,
                         dut.noc.bus_to_noc_resp_data[2].data,
                         dut.noc.gen_ports[2].mbus.rx_mask_reg);
                dbg_bus_to_noc_plo_resp_fire_count <= dbg_bus_to_noc_plo_resp_fire_count + 1;
            end

            if ((dbg_hddu_plo_resp_count < 16)
                && dut.noc.noc_plo_out_valid
                && dut.noc.noc_plo_out_ready) begin
                $display("[TB][DBG_EVT] hddu_plo_resp[%0d] status=%0d data0=0x%016h",
                         dbg_hddu_plo_resp_count,
                         dut.noc.noc_plo_out_status,
                         dut.noc.noc_plo_out_data[63:0]);
                dbg_hddu_plo_resp_count <= dbg_hddu_plo_resp_count + 1;
            end
        end
    end

    initial begin
        power_enable_i   = 1'b0;
        cmd_req_valid_i  = 1'b0;
        cmd_req_write_i  = 1'b0;
        cmd_req_addr_i   = 32'h0;
        cmd_req_wdata_i  = 32'h0;
        cmd_req_wstrb_i  = 4'h0;
        s_axi_awvalid_i  = 1'b0;
        s_axi_awaddr_i   = 32'h0;
        s_axi_wvalid_i   = 1'b0;
        s_axi_wdata_i    = 64'h0;
        s_axi_wstrb_i    = 8'h0;
        s_axi_bready_i   = 1'b0;
        s_axi_arvalid_i  = 1'b0;
        s_axi_araddr_i   = 32'h0;
        s_axi_rready_i   = 1'b0;
        hsel_i           = 1'b0;
        haddr_i          = 32'h0;
        hwrite_i         = 1'b0;
        htrans_i         = 2'b00;
        hsize_i          = 3'b010;
        hburst_i         = 3'b000;
        hprot_i          = 4'b0011;
        hready_i         = 1'b1;
        hwdata_i         = 32'h0;

        void'($value$plusargs("DATA_DIR=%s", data_dir));
        load_config({data_dir, "/rtl_cluster_case.cfg"});
        load_case_artifacts();

        @(posedge reset_n);
        @(posedge clk);
        @(negedge clk);
        power_enable_i = 1'b1;
        @(posedge clk);
        @(negedge clk);

        ahb_write(CLUSTER_MMIO_BASE + CLUSTER_REG_MODE, MODE_LAYER_MANAGED);
        config_spm_map(8'hE4);
        configure_scan_chain();
        load_pe_program_into_cluster();
        start_pe_only();
        repeat (4) @(posedge clk);
        $display("[TB][DBG] initial pe_busy port0.pe0=%0b port1.pe0=%0b port2.pe0=%0b",
                 dut.noc.pe_busy_sig[0][0],
                 dut.noc.pe_busy_sig[1][0],
                 dut.noc.pe_busy_sig[2][0]);

        begin
            bit wave_success;
            int exec_count;

            exec_count = (plan_count < wave_count) ? plan_count : wave_count;
            if (plan_count != wave_count) begin
                $display("[TB] plan_count=%0d differs from wave_count=%0d, executing %0d paired entries",
                         plan_count, wave_count, exec_count);
            end

            for (int wave_idx = 0; wave_idx < exec_count; wave_idx++) begin
                execute_wave(wave_idx, wave_success);
                if (!wave_success) begin
                    fail_count = fail_count + 1;
                    break;
                end
                pass_count = pass_count + 1;
            end

            stop_pe_only();

            if (fail_count == 0)
                verify_output_region();
        end

        `TB_SUMMARY("tb_cluster_sim_advanced")
        if (fail_count > 0)
            $fatal(1, "[TB] tb_cluster_sim_advanced failed");
        $finish;
    end

    initial begin
        repeat (MAX_SIM_CYCLES) @(posedge clk);
        $error("[TB_TIMEOUT] tb_cluster_sim_advanced exceeded %0d cycles", MAX_SIM_CYCLES);
        `TB_SUMMARY("tb_cluster_sim_advanced")
        $fatal(1, "[TB] tb_cluster_sim_advanced timeout");
    end
endmodule