//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/03/28
// Design Name:   HybridAcc Testbench
// Module Name:   tb_pe_sim
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   PE system-level simulation testbench (conv2d).
//                Faithfully translated from ESL (test_pe_sim.cpp).
//                Uses separate initial blocks per channel (mirrors SC_THREAD)
//                with event-based synchronization (mirrors sc_event).
// Dependencies:  tb_common.svh, hybridacc_utils_pkg.sv, PE/*.sv
// Revision:
//   2026/03/28 - Initial version
//   2026/03/29 - Complete rewrite from ESL reference; fix ICPSD errors
//-----------------------------------------------------------------------------
`include "../tb_common.svh"
`include "../../src/hybridacc_utils_pkg.sv"
`include "../../src/FIFO.sv"
`include "../../src/asyncFIFO.sv"
`include "../../src/PE/InstructionMemory.sv"
`include "../../src/PE/LoopController.sv"
`include "../../src/PE/Decoder.sv"
`include "../../src/PE/VADDU.sv"
`include "../../src/PE/VMULU.sv"
`include "../../src/PE/TransformRegFile.sv"
`include "../../src/PE/PsumRegFile.sv"
`include "../../src/PE/SRAM_SP_BWEB.sv"
`include "../../src/PE/DataMemory.sv"
`include "../../src/PE/LDMA.sv"
`include "../../src/PE/SDMA.sv"
`include "../../src/PE/IF_ID_Stage.sv"
`include "../../src/PE/EXE_M_Stage.sv"
`include "../../src/PE/EXE_A_Stage.sv"
`include "../../src/PE/PErouter.sv"
`include "../../src/PE/ProcessElement.sv"

module tb_pe_sim;
    import hybridacc_utils_pkg::*;

    // =====================================================================
    // Parameters (matching ESL test_pe_sim.cpp)
    // =====================================================================
    localparam int DEFAULT_CLOCK_PERIOD_NS = 10;
    localparam int PE_GAP_CYCLES           = 1;
    localparam longint unsigned MAX_WAIT_CYCLES = 200000;

    // =====================================================================
    // Types
    // =====================================================================
    typedef struct {
        int kernel_size;
        int in_ch;
        int out_ch;
        int out_width;
        int in_width;
        int groups_per_output;
    } conv_params_t;

    typedef struct {
        int  total_elements;
        int  mismatches;
        real cosine_similarity;
        real max_diff;
        real mse;
    } verify_stats_t;

    // =====================================================================
    // Configuration (from +plusargs)
    // =====================================================================
    string data_dir        = "output/pe-sim/conv_k3c4";
    real   verify_tolerance = 0.01;
    int    clock_period_ns  = DEFAULT_CLOCK_PERIOD_NS;

    // =====================================================================
    // Clock & reset
    // =====================================================================
    logic clk;
    logic reset_n;

    // =====================================================================
    // TB-driven signals — each driven by exactly ONE initial block
    // =====================================================================
    // test_main owns: router control, LN interfaces
    logic          router_enable;
    PERouterMode   router_mode;
    logic [63:0]   ln_pli_data;
    logic          ln_pli_valid;
    logic          ln_plo_ready;

    // ps_sender_thread owns: PS channel
    noc_request_t  noc_ps_in_data;
    logic          noc_ps_in_valid;

    // pd_sender_thread owns: PD channel
    noc_request_t  noc_pd_in_data;
    logic          noc_pd_in_valid;

    // pli_sender_thread owns: PLI channel
    noc_request_t  noc_pli_in_data;
    logic          noc_pli_in_valid;

    // plo_request_thread owns: PLO request channel
    noc_addr_req_t noc_plo_in_data;
    logic          noc_plo_in_valid;

    // plo_response_sink_thread owns: PLO response ready
    logic          noc_plo_out_ready;

    // =====================================================================
    // DUT output signals — structurally driven only (wire)
    // =====================================================================
    wire                noc_ps_in_ready;
    wire                noc_pd_in_ready;
    wire                noc_pli_in_ready;
    wire                noc_plo_in_ready;
    wire                noc_plo_out_valid;
    noc_response_t      noc_plo_out_data;
    wire                pe_busy;
    wire                ln_pli_ready;
    wire [63:0]         ln_plo_data;
    wire                ln_plo_valid;

    // =====================================================================
    // Shared data (written by test_main, read by traffic threads)
    // =====================================================================
    conv_params_t    conv;
    logic [15:0]     program_bin [];
    logic [63:0]     weights [];
    logic [15:0]     activations [];
    logic [63:0]     ps_inputs [];
    logic [15:0]     expected_fp16 [];
    logic [63:0]     received_vectors [$];
    int              pe_active_cycles;

    // =====================================================================
    // Synchronization events (mirrors ESL sc_event)
    // =====================================================================
    event ev_ps_program;
    event ev_ps_program_done;
    event ev_ps_start;
    event ev_ps_start_done;
    event ev_start_traffic;

    // =====================================================================
    // DUT instantiation
    // =====================================================================
    ProcessElement dut (
        .clk              (clk),
        .reset_n          (reset_n),
        .router_enable    (router_enable),
        .router_mode      (router_mode),
        .noc_ps_in_data   (noc_ps_in_data),
        .noc_ps_in_valid  (noc_ps_in_valid),
        .noc_ps_in_ready  (noc_ps_in_ready),
        .noc_pd_in_data   (noc_pd_in_data),
        .noc_pd_in_valid  (noc_pd_in_valid),
        .noc_pd_in_ready  (noc_pd_in_ready),
        .noc_pli_in_data  (noc_pli_in_data),
        .noc_pli_in_valid (noc_pli_in_valid),
        .noc_pli_in_ready (noc_pli_in_ready),
        .noc_plo_in_data  (noc_plo_in_data),
        .noc_plo_in_valid (noc_plo_in_valid),
        .noc_plo_in_ready (noc_plo_in_ready),
        .noc_plo_out_data (noc_plo_out_data),
        .noc_plo_out_valid(noc_plo_out_valid),
        .noc_plo_out_ready(noc_plo_out_ready),
        .pe_busy          (pe_busy),
        .ln_pli_data      (ln_pli_data),
        .ln_pli_valid     (ln_pli_valid),
        .ln_pli_ready     (ln_pli_ready),
        .ln_plo_data      (ln_plo_data),
        .ln_plo_valid     (ln_plo_valid),
        .ln_plo_ready     (ln_plo_ready)
    );

    // =====================================================================
    // Helper functions (pure computation, no signal driving)
    // =====================================================================

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
        input logic [15:0] values [],
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

    // Matches ESL verify_fp16_vectors (tb_utils.hpp)
    function automatic verify_stats_t verify_fp16_vectors(
        input logic [15:0] expected [],
        input logic [15:0] received [],
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

            // ESL: size mismatch or empty → immediate failure
            if ((expected.size() != received.size()) || (expected.size() == 0)) begin
                stats.mismatches = (expected.size() > received.size())
                                    ? expected.size() : received.size();
                stats.cosine_similarity = 0.0;
                stats.max_diff = 1.0e30;
                stats.mse      = 1.0e30;
                verify_fp16_vectors = stats;
                return;
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
                if (diff < 0.0) diff = -diff;
                total_sq_err += diff * diff;
                if (diff > stats.max_diff)
                    stats.max_diff = diff;
                if (diff > tolerance) begin
                    stats.mismatches++;
                    $display("[Mismatch] Index %0d: Expected %f (0x%04h), Received %f (0x%04h), Diff %e",
                        i, expected_real, expected[i], received_real, received[i], diff);
                end
            end

            mag_expected = $sqrt(mag_expected);
            mag_received = $sqrt(mag_received);
            stats.mse    = total_sq_err / total;

            // ESL: returns 0.0 when magnitudes are zero
            if ((mag_expected > 0.0) && (mag_received > 0.0))
                stats.cosine_similarity = dot_product / (mag_expected * mag_received);
            else
                stats.cosine_similarity = 0.0;

            stats.total_elements    = total;
            verify_fp16_vectors     = stats;
        end
    endfunction

    // =====================================================================
    // File I/O tasks (pure data, no signal driving)
    // =====================================================================

    task automatic read_binary_bytes(input string path, output byte unsigned data[]);
        int fd, size, count;
        begin
            fd = $fopen(path, "rb");
            if (fd == 0) $fatal(1, "[TB] Failed to open %s", path);
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
        byte unsigned raw [];
        int count;
        begin
            read_binary_bytes(path, raw);
            `TB_ASSERT((raw.size() % 2) == 0, "u16 binary size must be even")
            count = raw.size() / 2;
            data  = new[count];
            for (int i = 0; i < count; i++)
                data[i] = {raw[(i * 2) + 1], raw[i * 2]};
        end
    endtask

    task automatic read_binary_u64(input string path, output logic [63:0] data[]);
        byte unsigned raw [];
        int count;
        begin
            read_binary_bytes(path, raw);
            `TB_ASSERT((raw.size() % 8) == 0, "u64 binary size must be multiple of 8")
            count = raw.size() / 8;
            data  = new[count];
            for (int i = 0; i < count; i++)
                data[i] = {
                    raw[(i*8)+7], raw[(i*8)+6], raw[(i*8)+5], raw[(i*8)+4],
                    raw[(i*8)+3], raw[(i*8)+2], raw[(i*8)+1], raw[(i*8)+0]
                };
        end
    endtask

    task automatic parse_conv_params_from_meta(input string meta_path, output conv_params_t params);
        int    fd, code;
        string line;
        begin
            params = '{0, 0, 0, 0, 0, 0};
            fd = $fopen(meta_path, "r");
            if (fd == 0) $fatal(1, "[TB] Failed to open %s", meta_path);
            while (!$feof(fd)) begin
                line = "";
                code = $fgets(line, fd);
                if (code == 0) continue;
                if ($sscanf(line, "kernel_size=%d", params.kernel_size) == 1) continue;
                if ($sscanf(line, "kernel_size: %d", params.kernel_size) == 1) continue;
                if ($sscanf(line, "in_ch=%d",        params.in_ch)       == 1) continue;
                if ($sscanf(line, "in_ch: %d",       params.in_ch)       == 1) continue;
                if ($sscanf(line, "out_ch=%d",       params.out_ch)      == 1) continue;
                if ($sscanf(line, "out_ch: %d",      params.out_ch)      == 1) continue;
                if ($sscanf(line, "out_width=%d",    params.out_width)   == 1) continue;
                if ($sscanf(line, "out_width: %d",   params.out_width)   == 1) continue;
                if ($sscanf(line, "in_width=%d",     params.in_width)    == 1) continue;
                if ($sscanf(line, "in_width: %d",    params.in_width)    == 1) continue;
            end
            $fclose(fd);
            if (params.out_ch > 0)
                params.groups_per_output = params.out_ch / 4;
        end
    endtask

    task automatic load_test_data_conv2d;
        begin
            parse_conv_params_from_meta({data_dir, "/meta.txt"}, conv);
            `TB_ASSERT(conv.kernel_size > 0 && conv.in_ch > 0 && conv.out_ch > 0 &&
                       conv.out_width > 0 && conv.in_width > 0 && conv.groups_per_output > 0,
                       "Invalid conv params from meta.txt")

            read_binary_u16({data_dir, "/pe_program.bin"},       program_bin);
            read_binary_u64({data_dir, "/weight.bin"},           weights);
            read_binary_u16({data_dir, "/activation_input.bin"}, activations);
            read_binary_u64({data_dir, "/ps_input.bin"},         ps_inputs);
            read_binary_u16({data_dir, "/activation_output.bin"},expected_fp16);

            $display("[TB] Loaded data from %s", data_dir);
            $display("  Program: %0d instructions",  program_bin.size());
            $display("  Weights: %0d x uint64",      weights.size());
            $display("  Activations: %0d x fp16",    activations.size());
            $display("  PS Inputs: %0d x uint64",    ps_inputs.size());
            $display("  Expected: %0d x fp16",       expected_fp16.size());
            $display("[TB] Meta: kernel_size=%0d in_ch=%0d out_ch=%0d out_width=%0d in_width=%0d groups_per_output=%0d",
                conv.kernel_size, conv.in_ch, conv.out_ch, conv.out_width,
                conv.in_width, conv.groups_per_output);
        end
    endtask

    // =====================================================================
    // Per-channel send tasks
    // Each directly references its own channel signals — no DUT output
    // wires passed as task arguments (avoids ICPSD).
    // Handshake protocol: assert valid+data, wait for ready at negedge
    // (ensures DUT's posedge NBA updates have settled), then deassert
    // at next posedge. Mirrors ESL's wait(SC_ZERO_TIME) after posedge.
    // MUST only be called from the owning initial block.
    // =====================================================================

    task automatic send_ps_req(input noc_request_t req);
        noc_ps_in_data  <= req;
        noc_ps_in_valid <= 1'b1;
        forever begin
            @(posedge clk);
            if (noc_ps_in_ready) break;
        end
        noc_ps_in_valid <= 1'b0;
        noc_ps_in_data  <= '0;
    endtask

    task automatic send_pd_req(input noc_request_t req);
        noc_pd_in_data  <= req;
        noc_pd_in_valid <= 1'b1;
        forever begin
            @(posedge clk);
            if (noc_pd_in_ready) break;
        end
        noc_pd_in_valid <= 1'b0;
        noc_pd_in_data  <= '0;
    endtask

    task automatic send_pli_req(input noc_request_t req);
        noc_pli_in_data  <= req;
        noc_pli_in_valid <= 1'b1;
        forever begin
            @(posedge clk);
            if (noc_pli_in_ready) break;
        end
        noc_pli_in_valid <= 1'b0;
        noc_pli_in_data  <= '0;
    endtask

    task automatic send_plo_addr_req(input noc_addr_req_t req);
        noc_plo_in_data  <= req;
        noc_plo_in_valid <= 1'b1;
        forever begin
            @(posedge clk);
            if (noc_plo_in_ready) break;
        end
        noc_plo_in_valid <= 1'b0;
        noc_plo_in_data  <= '0;
    endtask

    task automatic wait_cycles(input int n);
        repeat (n) @(posedge clk);
    endtask

    // =====================================================================
    // Plusargs
    // =====================================================================
    initial begin
        void'($value$plusargs("DATA_DIR=%s",       data_dir));
        void'($value$plusargs("VERIFY_TOL=%f",     verify_tolerance));
        void'($value$plusargs("CLOCK_PERIOD_NS=%d", clock_period_ns));
    end

    // =====================================================================
    // Clock generation
    // =====================================================================
    initial begin
        clk = 1'b0;
        forever #(clock_period_ns / 2) clk = ~clk;
    end

    // =====================================================================
    // Test Main Thread (mirrors ESL test_main SC_THREAD)
    // Drives: reset_n, router_enable, router_mode, ln_pli_*, ln_plo_ready
    // =====================================================================
    initial begin : test_main
        router_enable = 1'b0;
        router_mode   = PLI_FROM_BUS_PLO_TO_BUS;
        ln_pli_data   = '0;
        ln_pli_valid  = 1'b0;
        ln_plo_ready  = 1'b1;
        received_vectors.delete();

        // Reset (mirrors ESL reset_dut(10))
        reset_n = 1'b0;
        repeat (10) @(posedge clk);
        reset_n = 1'b1;

        $display("========================================");
        $display("PE Simulation Test (conv2d)");
        $display("========================================");

        router_enable = 1'b1;
        router_mode   = PLI_FROM_BUS_PLO_TO_BUS;
        @(posedge clk);

        // Load test data (pure data, no signal driving)
        load_test_data_conv2d();
        `TB_ASSERT(program_bin.size() > 0, "Program must not be empty")

        // Step 1: Program PE via PS (delegated to ps_sender_thread)
        $display("\n[Step 1] Programming PE...");
        -> ev_ps_program;
        @(ev_ps_program_done);

        // Step 2: Start PE via PS (delegated to ps_sender_thread)
        $display("\n[Step 2] Starting PE...");
        -> ev_ps_start;
        @(ev_ps_start_done);

        // Step 3: Start all traffic threads concurrently
        $display("\n[Step 3] Starting traffic threads...");
        -> ev_start_traffic;

        // Wait for all output vectors (or timeout)
        begin
            longint unsigned waited;
            int expected_vectors;
            expected_vectors = conv.out_width * conv.groups_per_output;
            waited = 0;
            while ((received_vectors.size() < expected_vectors) && (waited < MAX_WAIT_CYCLES)) begin
                @(posedge clk);
                waited++;
                if ((waited % 20000) == 0)
                    $display("[TB] Waiting outputs... cycles=%0d received=%0d/%0d pe_busy=%0b",
                        waited, received_vectors.size(), expected_vectors, pe_busy);
            end
            if (received_vectors.size() < expected_vectors) begin
                $display("[TB] Output timeout: received=%0d expected=%0d",
                    received_vectors.size(), expected_vectors);
                $fatal(1, "[TB] PE output timeout");
            end else begin
                $display("[TB] Outputs collected: %0d vectors", received_vectors.size());
            end
        end

        // Step 4: Wait for PE halt (bounded)
        $display("\n[Step 4] Waiting for PE halt...");
        begin
            int halt_waited;
            halt_waited = 0;
            while (pe_busy && (halt_waited < 200)) begin
                @(posedge clk);
                halt_waited++;
            end
            $display("[TB] PE halted=%s after %0d cycles",
                pe_busy ? "No" : "Yes", halt_waited);
        end

        // Step 5: Verification (matches ESL verify_fp16_vectors)
        $display("\n[Step 5] Verifying outputs...");
        begin
            logic [15:0] received_fp16 [];
            verify_stats_t stats;
            int total;

            // Flatten received 64-bit vectors to fp16 array
            total = received_vectors.size() * 4;
            received_fp16 = new[total];
            for (int i = 0; i < received_vectors.size(); i++)
                for (int lane = 0; lane < 4; lane++)
                    received_fp16[(i * 4) + lane] = received_vectors[i][(lane * 16) +: 16];

            $display("Expected fp16: %0d", expected_fp16.size());
            $display("Received fp16: %0d", received_fp16.size());

            stats = verify_fp16_vectors(expected_fp16, received_fp16, verify_tolerance);
            $display("----------------------------------------");
            $display("Verification Results:");
            $display("  Total Elements: %0d",                stats.total_elements);
            $display("  Mismatches: %0d (Tolerance: %f)",    stats.mismatches, verify_tolerance);
            $display("  Cosine Similarity: %f",              stats.cosine_similarity);
            $display("  Max Difference: %e",                 stats.max_diff);
            $display("  MSE: %e",                            stats.mse);
            $display("----------------------------------------");

            if ((stats.cosine_similarity > 0.99) && (stats.mismatches == 0)) begin
                $display("PASS: tb_pe_sim");
            end else begin
                $display("FAIL: tb_pe_sim");
                $fatal(1, "[TB] Verification failed");
            end
        end

        // Step 6: Performance metrics
        $display("\n[Step 6] Performance Metrics:");
        $display("  Active Cycles: %0d", pe_active_cycles);

        $display("\n========================================");
        $display("Test completed.");
        $display("========================================");
        $finish;
    end

    // =====================================================================
    // PS Sender Thread (mirrors ESL ps_sender SC_THREAD)
    // Drives: noc_ps_in_data, noc_ps_in_valid
    // Reads:  noc_ps_in_ready (wire, DUT output)
    //
    // Handles 3 phases: program PE → start PE → stream weights
    // =====================================================================
    initial begin : ps_sender_thread
        noc_request_t req;
        logic [63:0]  cmd;

        noc_ps_in_data  = '0;
        noc_ps_in_valid = 1'b0;

        // Wait for reset release (mirrors ESL: while(!reset_n) wait(...))
        @(posedge reset_n);

        // ---- Phase 1: Program PE instructions ----
        @(ev_ps_program);
        for (int i = 0; i < program_bin.size(); i++) begin
            cmd = 64'd2;                              // CMD_LOAD_PROGRAM
            cmd |= (64'(i * 2) << 4);                // byte addr
            cmd |= (64'(program_bin[i]) << 16);       // instruction
            req.addr = 16'h0040;
            req.data = cmd;
            req.mask = '0;
            send_ps_req(req);
        end
        -> ev_ps_program_done;

        // ---- Phase 2: Start PE ----
        @(ev_ps_start);
        req.addr = 16'h0040;
        req.data = 64'd4;                            // CMD_START_PE
        req.mask = '0;
        send_ps_req(req);
        wait_cycles(5);
        -> ev_ps_start_done;

        // ---- Phase 3: Stream weights ----
        @(ev_start_traffic);
        $display("[PS] Streaming weights...");
        for (int i = 0; i < weights.size(); i++) begin
            req.addr = 16'h0000;
            req.data = weights[i];
            req.mask = '0;
            send_ps_req(req);
            if ((i % 256) == 0)
                $display("[PS] Sent weights %0d/%0d", i, weights.size());
            wait_cycles(PE_GAP_CYCLES);
        end
        $display("[PS] Completed streaming weights.");
    end

    // =====================================================================
    // PD Sender Thread (mirrors ESL pd_sender SC_THREAD)
    // Drives: noc_pd_in_data, noc_pd_in_valid
    // Reads:  noc_pd_in_ready (wire, DUT output)
    // =====================================================================
    initial begin : pd_sender_thread
        noc_pd_in_data  = '0;
        noc_pd_in_valid = 1'b0;

        @(posedge reset_n);
        @(ev_start_traffic);

        $display("[PD] Streaming partial dot-product positions...");
        for (int in_pos = 0; in_pos < conv.in_width; in_pos++) begin
            // Inline send_pd_position (mirrors ESL)
            begin
                noc_request_t req;
                int base, in_ch_step, idx;
                logic [7:0]  mask;
                logic [63:0] packed_v;

                base = in_pos * conv.in_ch;
                case (conv.kernel_size)
                    3:       in_ch_step = 4;
                    5:       in_ch_step = 2;
                    7:       in_ch_step = 1;
                    default: begin
                        in_ch_step = 4;
                        $display("[PD] Warning: unexpected kernel_size=%0d",
                            conv.kernel_size);
                    end
                endcase

                for (int c = 0; c < conv.in_ch; c += in_ch_step) begin
                    idx = base + c;
                    if (idx >= activations.size()) break;
                    packed_v = pack_fp16x4(activations, idx, in_ch_step, mask);
                    if (mask == 0) break;
                    req.addr = 16'h0040;
                    req.data = packed_v;
                    req.mask = 64'(mask);
                    send_pd_req(req);
                end
            end
            wait_cycles(PE_GAP_CYCLES);
        end
        $display("[PD] Completed streaming PD positions.");
    end

    // =====================================================================
    // PLI Sender Thread (mirrors ESL pli_sender SC_THREAD)
    // Drives: noc_pli_in_data, noc_pli_in_valid
    // Reads:  noc_pli_in_ready (wire, DUT output)
    // =====================================================================
    initial begin : pli_sender_thread
        noc_request_t req;
        int expected_vectors, idx;

        noc_pli_in_data  = '0;
        noc_pli_in_valid = 1'b0;

        @(posedge reset_n);
        @(ev_start_traffic);

        expected_vectors = conv.out_width * conv.groups_per_output;
        if (ps_inputs.size() < expected_vectors)
            $display("[PLI] Warning: ps_inputs smaller than expected (%0d < %0d)",
                ps_inputs.size(), expected_vectors);

        begin : pli_loop
            for (int out_pos = 0; out_pos < conv.out_width; out_pos++) begin
                for (int g = 0; g < conv.groups_per_output; g++) begin
                    idx = (out_pos * conv.groups_per_output) + g;
                    if (idx >= ps_inputs.size()) disable pli_loop;
                    req.addr = 16'h0080;
                    req.data = ps_inputs[idx];
                    req.mask = '0;
                    send_pli_req(req);
                    wait_cycles(PE_GAP_CYCLES);
                end
            end
        end
    end

    // =====================================================================
    // PLO Request Thread (mirrors ESL plo_request_thread SC_THREAD)
    // Drives: noc_plo_in_data, noc_plo_in_valid
    // Reads:  noc_plo_in_ready (wire, DUT output)
    // =====================================================================
    initial begin : plo_request_thread
        noc_addr_req_t req;
        int expected_vectors;

        noc_plo_in_data  = '0;
        noc_plo_in_valid = 1'b0;

        @(posedge reset_n);
        @(ev_start_traffic);

        expected_vectors = conv.out_width * conv.groups_per_output;
        req.addr = 16'h00C0;
        for (int i = 0; i < expected_vectors; i++) begin
            send_plo_addr_req(req);
            if (((i + 1) % 256) == 0)
                $display("[PLO-REQ] Issued %0d/%0d read requests",
                    i + 1, expected_vectors);
        end
    end

    // =====================================================================
    // PLO Response Sink Thread (mirrors ESL plo_response_sink SC_THREAD)
    // Drives: noc_plo_out_ready
    // Reads:  noc_plo_out_valid, noc_plo_out_data (DUT outputs)
    // =====================================================================
    initial begin : plo_response_sink_thread
        int expected_vectors;

        noc_plo_out_ready = 1'b0;

        @(posedge reset_n);
        @(ev_start_traffic);

        expected_vectors  = conv.out_width * conv.groups_per_output;
        noc_plo_out_ready = 1'b1;

        while (received_vectors.size() < expected_vectors) begin
            @(posedge clk);
            if (noc_plo_out_valid && noc_plo_out_ready) begin
                if (noc_plo_out_data.status == NOC_OK) begin
                    received_vectors.push_back(noc_plo_out_data.data);
                    if ((received_vectors.size() % 256) == 0)
                        $display("[PLO-RSP] Received %0d/%0d vectors",
                            received_vectors.size(), expected_vectors);
                end
            end
        end
        noc_plo_out_ready = 1'b0;
    end

    // =====================================================================
    // Active cycle counter
    // =====================================================================
    always @(posedge clk) begin
        if (!reset_n)
            pe_active_cycles <= 0;
        else if (pe_busy)
            pe_active_cycles <= pe_active_cycles + 1;
    end

    // =====================================================================
    // Debug: monitor internal PErouter FIFO push/pop and PE pipeline signals
    // =====================================================================
    int dbg_ps_push_cnt, dbg_ps_pop_cnt;
    int dbg_pd_push_cnt, dbg_pd_pop_cnt, dbg_pd_pop_set_cnt;
    int dbg_pli_push_cnt, dbg_pli_pop_cnt;
    int dbg_plo_push_cnt, dbg_plo_pop_cnt;

    always @(posedge clk) begin
        if (!reset_n) begin
            dbg_ps_push_cnt  <= 0;
            dbg_ps_pop_cnt   <= 0;
            dbg_pd_push_cnt  <= 0;
            dbg_pd_pop_cnt   <= 0;
            dbg_pd_pop_set_cnt <= 0;
            dbg_pli_push_cnt <= 0;
            dbg_pli_pop_cnt  <= 0;
            dbg_plo_push_cnt <= 0;
            dbg_plo_pop_cnt  <= 0;
        end else begin
            if (dut.router.ps_push)    dbg_ps_push_cnt    <= dbg_ps_push_cnt  + 1;
            if (dut.router.ps_pop)     dbg_ps_pop_cnt     <= dbg_ps_pop_cnt   + 1;
            if (dut.router.pd_push)    dbg_pd_push_cnt    <= dbg_pd_push_cnt  + 1;
            if (dut.router.pd_pop)     dbg_pd_pop_cnt     <= dbg_pd_pop_cnt   + 1;
            if (dut.router.pd_pop_set) dbg_pd_pop_set_cnt <= dbg_pd_pop_set_cnt + 1;
            if (dut.router.pli_push)   dbg_pli_push_cnt   <= dbg_pli_push_cnt + 1;
            if (dut.router.pli_pop)    dbg_pli_pop_cnt    <= dbg_pli_pop_cnt  + 1;
            if (dut.router.plo_push)   dbg_plo_push_cnt   <= dbg_plo_push_cnt + 1;
            if (dut.router.plo_pop)    dbg_plo_pop_cnt    <= dbg_plo_pop_cnt  + 1;
        end
    end

    // Print summary at the end of simulation
    final begin
        $display("\n[DBG] FIFO Push/Pop Summary:");
        $display("  PS  push=%0d pop=%0d", dbg_ps_push_cnt, dbg_ps_pop_cnt);
        $display("  PD  push=%0d pop=%0d pop_set=%0d", dbg_pd_push_cnt, dbg_pd_pop_cnt, dbg_pd_pop_set_cnt);
        $display("  PLI push=%0d pop=%0d", dbg_pli_push_cnt, dbg_pli_pop_cnt);
        $display("  PLO push=%0d pop=%0d", dbg_plo_push_cnt, dbg_plo_pop_cnt);
    end

    // Debug: SWAP instruction trace
    always @(posedge clk) begin
        if (reset_n && dut.exe_m_stage.decode_reg.is_swap)
            $display("[DBG-SWAP] t=%0t EXE_M: is_swap=1 valid_reg=%b ready_in=%b sdma_busy=%b sdma_swap=%b sdma_st=%0d stall_DL=%b swap_stall=%b",
                $time, dut.exe_m_stage.valid_reg, dut.exe_m_stage.ready_in,
                dut.exe_m_stage.sdma_busy, dut.exe_m_stage.sdma_swap,
                dut.exe_m_stage.sdma.state_reg,
                dut.exe_m_stage.stall_DL, dut.exe_m_stage.swap_stall);
        if (reset_n && dut.if_id_stage.decoder_decode_signals_out_sig.is_swap)
            $display("[DBG-SWAP] t=%0t IF_ID decoder: is_swap=1 valid=%b ready_in=%b pc=%0d",
                $time, dut.if_id_stage.valid_reg, dut.if_id_stage.ready_in, dut.if_id_stage.pc_reg);
    end

    // Debug: SDMA state transitions
    logic [1:0] dbg_last_sdma_st;
    initial dbg_last_sdma_st = 0;
    always @(posedge clk) begin
        if (reset_n && dut.exe_m_stage.sdma.state_reg != dbg_last_sdma_st) begin
            $display("[DBG-SDMA] t=%0t state %0d -> %0d bank_sel=%b swap_in=%b",
                $time, dbg_last_sdma_st, dut.exe_m_stage.sdma.state_reg,
                dut.exe_m_stage.sdma.bank_sel, dut.exe_m_stage.sdma.swap_in);
            dbg_last_sdma_st <= dut.exe_m_stage.sdma.state_reg;
        end
    end

    // Debug: periodic pipeline status every 50000 cycles after swap
    int dbg_periodic_cnt;
    initial dbg_periodic_cnt = 0;
    always @(posedge clk) begin
        if (reset_n && dbg_periodic_cnt < 20) begin
            if ($time > 1500000 && ($time % 500000 < 10001)) begin
                dbg_periodic_cnt <= dbg_periodic_cnt + 1;
                $display("[DBG-STATUS] t=%0t", $time);
                $display("  IF_ID: pc=%0d valid=%b halted=%b ready_in=%b",
                    dut.if_id_stage.pc_reg, dut.if_id_stage.valid_reg,
                    dut.if_id_stage.halted_reg, dut.if_id_stage.ready_in);
                $display("  EXE_M: valid=%b ready_out=%b ready_in=%b stall_DL=%b stall_PS=%b stall_PD=%b swap_stall=%b",
                    dut.exe_m_stage.valid_reg, dut.exe_m_stage.ready_out,
                    dut.exe_m_stage.ready_in, dut.exe_m_stage.stall_DL,
                    dut.exe_m_stage.stall_PS, dut.exe_m_stage.stall_PD,
                    dut.exe_m_stage.swap_stall);
                $display("  EXE_M decode: is_swap=%b pd_load=%b pd_load_v=%b sys_sdma_act=%b LDMA_next=%b vaddu_en=%b",
                    dut.exe_m_stage.decode_reg.is_swap, dut.exe_m_stage.decode_reg.pd_load,
                    dut.exe_m_stage.decode_reg.pd_load_v, dut.exe_m_stage.decode_reg.sys_sdma_act,
                    dut.exe_m_stage.decode_reg.LDMA_next, dut.exe_m_stage.decode_reg.vaddu_en);
                $display("  EXE_A: state=%0d valid_in=%b ready_out=%b",
                    dut.exe_a_stage.state_reg, dut.exe_a_stage.valid_in,
                    dut.exe_a_stage.ready_out);
                $display("  FIFO: pd_valid=%b pd_set_valid=%b pli_valid=%b plo_ready=%b ps_valid=%b",
                    dut.exe_m_stage.pd_valid, dut.exe_m_stage.pd_set_valid,
                    dut.exe_a_stage.pli_valid, dut.exe_a_stage.plo_ready,
                    dut.exe_m_stage.ps_valid);
                $display("  LDMA: state=%0d next=%b busy=%b done=%b dl_stall=%b base=%0d off=%0d len=%0d req_type=%0d bcast=%b stride=%0d",
                    dut.exe_m_stage.ldma.state_reg, dut.exe_m_stage.ldma_next,
                    dut.exe_m_stage.ldma_busy, dut.exe_m_stage.ldma_done,
                    dut.exe_m_stage.ldma_stall,
                    dut.exe_m_stage.ldma.dma_base_reg, dut.exe_m_stage.ldma.dma_offset_reg,
                    dut.exe_m_stage.ldma.dma_len_reg,
                    dut.exe_m_stage.ldma.request_type_reg, dut.exe_m_stage.ldma.dma_broadcast_reg,
                    dut.exe_m_stage.ldma.dma_stride_reg);
            end
        end
    end

    // Debug: trace first 20 PLO outputs to understand multiply-accumulate behavior
    int dbg_plo_trace_cnt;
    initial dbg_plo_trace_cnt = 0;
    always @(posedge clk) begin
        if (reset_n && dut.router.plo_push && dbg_plo_trace_cnt < 20) begin
            dbg_plo_trace_cnt <= dbg_plo_trace_cnt + 1;
            $display("[DBG-PLO] push #%0d: data=0x%016h", dbg_plo_trace_cnt,
                dut.exe_a_stage.plo_data);
            $display("  EXE_A: vmul_reg={%h,%h,%h,%h} pli_reg={%h,%h,%h,%h} vaddu_result={%h,%h,%h,%h}",
                dut.exe_a_stage.vmul_data_reg.lanes[3], dut.exe_a_stage.vmul_data_reg.lanes[2],
                dut.exe_a_stage.vmul_data_reg.lanes[1], dut.exe_a_stage.vmul_data_reg.lanes[0],
                dut.exe_a_stage.pli_data_reg.lanes[3], dut.exe_a_stage.pli_data_reg.lanes[2],
                dut.exe_a_stage.pli_data_reg.lanes[1], dut.exe_a_stage.pli_data_reg.lanes[0],
                dut.exe_a_stage.vaddu_result_sig.lanes[3], dut.exe_a_stage.vaddu_result_sig.lanes[2],
                dut.exe_a_stage.vaddu_result_sig.lanes[1], dut.exe_a_stage.vaddu_result_sig.lanes[0]);
            $display("  EXE_A: pli_plo_op=%b state=%0d vaddu_mode=0x%08h pr_vp_out={%h,%h,%h,%h}",
                dut.exe_a_stage.decode_reg.pli_plo_operation,
                dut.exe_a_stage.state_reg,
                dut.exe_a_stage.decode_reg.vaddu_mode,
                dut.exe_a_stage.pr_vp_out.lanes[3], dut.exe_a_stage.pr_vp_out.lanes[2],
                dut.exe_a_stage.pr_vp_out.lanes[1], dut.exe_a_stage.pr_vp_out.lanes[0]);
        end
    end

    // Debug: trace VMAC pipeline — PR write operations and state transitions
    int dbg_vmac_trace_cnt;
    initial dbg_vmac_trace_cnt = 0;
    always @(posedge clk) begin
        if (reset_n && dbg_vmac_trace_cnt < 200) begin
            // Trace EVERY state transition for the first 200 cycles of PE execution
            if (dut.exe_a_stage.state_reg != 0 || dut.exe_a_stage.state_next != 0) begin
                dbg_vmac_trace_cnt <= dbg_vmac_trace_cnt + 1;
                $display("[DBG-FSM] t=%0t state=%0d->%0d valid_in=%b ready_out=%b",
                    $time, dut.exe_a_stage.state_reg, dut.exe_a_stage.state_next,
                    dut.exe_a_stage.valid_in, dut.exe_a_stage.ready_out);
                $display("  EXE_M: vmul_result={%h,%h,%h,%h} tr_vtid={%h,%h,%h,%h} ldma_dmrv={%h,%h,%h,%h}",
                    dut.exe_m_stage.vmul_result.lanes[3], dut.exe_m_stage.vmul_result.lanes[2],
                    dut.exe_m_stage.vmul_result.lanes[1], dut.exe_m_stage.vmul_result.lanes[0],
                    dut.exe_m_stage.tr_vtid_out.lanes[3], dut.exe_m_stage.tr_vtid_out.lanes[2],
                    dut.exe_m_stage.tr_vtid_out.lanes[1], dut.exe_m_stage.tr_vtid_out.lanes[0],
                    dut.exe_m_stage.ldma_dmrv_out.lanes[3], dut.exe_m_stage.ldma_dmrv_out.lanes[2],
                    dut.exe_m_stage.ldma_dmrv_out.lanes[1], dut.exe_m_stage.ldma_dmrv_out.lanes[0]);
                $display("  EXE_M: ps_data_vec={%h,%h,%h,%h} valid=%b DL=%b PS=%b PD=%b LDMA_next=%b ldma_st=%0d",
                    dut.exe_m_stage.ps_data_vec.lanes[3], dut.exe_m_stage.ps_data_vec.lanes[2],
                    dut.exe_m_stage.ps_data_vec.lanes[1], dut.exe_m_stage.ps_data_vec.lanes[0],
                    dut.exe_m_stage.valid_reg, dut.exe_m_stage.stall_DL,
                    dut.exe_m_stage.stall_PS, dut.exe_m_stage.stall_PD,
                    dut.exe_m_stage.decode_reg.LDMA_next, dut.exe_m_stage.ldma.state_reg);
                $display("  LDMA: dm_addr=0x%04h dm_data=0x%016h dmrv_reg={%h,%h,%h,%h} base=%0d off=%0d len=%0d",
                    dut.exe_m_stage.ldma.dm_read_addr, dut.exe_m_stage.ldma.dm_read_data,
                    dut.exe_m_stage.ldma.dmrv_reg.lanes[3], dut.exe_m_stage.ldma.dmrv_reg.lanes[2],
                    dut.exe_m_stage.ldma.dmrv_reg.lanes[1], dut.exe_m_stage.ldma.dmrv_reg.lanes[0],
                    dut.exe_m_stage.ldma.dma_base_reg, dut.exe_m_stage.ldma.dma_offset_reg,
                    dut.exe_m_stage.ldma.dma_len_reg);
                $display("  DM: bank_sel=%b sdma_st=%0d sdma_wr=%b sdma_swap=%b",
                    dut.exe_m_stage.sdma.bank_sel,
                    dut.exe_m_stage.sdma.state_reg,
                    dut.exe_m_stage.dm_write_en,
                    dut.exe_m_stage.sdma_swap);
                $display("  decode: vaddu_en=%b vaddu_mode=%0d pli_plo_op=%b pr_en=%b pr_write=%b pr_mode=%b pr_clear=%b pr_use_vc=%b pr_incr_vc=%b halt=%b",
                    dut.exe_a_stage.decode_reg.vaddu_en,
                    dut.exe_a_stage.decode_reg.vaddu_mode,
                    dut.exe_a_stage.decode_reg.pli_plo_operation,
                    dut.exe_a_stage.decode_reg.pr_en,
                    dut.exe_a_stage.decode_reg.pr_write,
                    dut.exe_a_stage.decode_reg.pr_mode,
                    dut.exe_a_stage.decode_reg.pr_clear_regs,
                    dut.exe_a_stage.decode_reg.pr_use_vcounter,
                    dut.exe_a_stage.decode_reg.pr_incr_vcounter,
                    dut.exe_a_stage.decode_reg.halt);
                $display("  PR: wr_en=%b mode=%0d pid=%0d use_pc=%b pcnt=%0d clr=%b incr=%b",
                    dut.exe_a_stage.pr_vpid_write_en,
                    dut.exe_a_stage.pr_mode,
                    dut.exe_a_stage.pr_pid,
                    dut.exe_a_stage.pr_use_pcounter,
                    dut.exe_a_stage.PR.pcounter_reg,
                    dut.exe_a_stage.pr_clear_regs,
                    dut.exe_a_stage.pr_incr_pcounter);
                $display("  PR: p_in=%h p_out=%h vp_out={%h,%h,%h,%h}",
                    dut.exe_a_stage.pr_p_in, dut.exe_a_stage.pr_p_out,
                    dut.exe_a_stage.pr_vp_out.lanes[3], dut.exe_a_stage.pr_vp_out.lanes[2],
                    dut.exe_a_stage.pr_vp_out.lanes[1], dut.exe_a_stage.pr_vp_out.lanes[0]);
                $display("  VMAC: s1_reg0=%h s1_reg1=%h s2_reg=%h vmul={%h,%h,%h,%h}",
                    dut.exe_a_stage.s1_reg0, dut.exe_a_stage.s1_reg1, dut.exe_a_stage.s2_reg_r,
                    dut.exe_a_stage.vmul_data_reg.lanes[3], dut.exe_a_stage.vmul_data_reg.lanes[2],
                    dut.exe_a_stage.vmul_data_reg.lanes[1], dut.exe_a_stage.vmul_data_reg.lanes[0]);
                if (dut.exe_a_stage.state_reg == 3'd2 || dut.exe_a_stage.state_reg == 3'd3 || dut.exe_a_stage.state_reg == 3'd4)
                    $display("  VMAC: decode_s2: pr_write=%b pr_en=%b pr_mode=%b pr_use_vc=%b pr_incr_vc=%b",
                        dut.exe_a_stage.decode_s2_reg.pr_write,
                        dut.exe_a_stage.decode_s2_reg.pr_en,
                        dut.exe_a_stage.decode_s2_reg.pr_mode,
                        dut.exe_a_stage.decode_s2_reg.pr_use_vcounter,
                        dut.exe_a_stage.decode_s2_reg.pr_incr_vcounter);
            end
        end
    end

endmodule