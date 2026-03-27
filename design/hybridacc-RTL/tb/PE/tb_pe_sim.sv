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

    localparam int DEFAULT_CLOCK_PERIOD_NS = 10;
    localparam int PE_GAP_CYCLES = 1;
    localparam longint unsigned MAX_WAIT_CYCLES = 200000;

    typedef struct {
        int kernel_size;
        int in_ch;
        int out_ch;
        int out_width;
        int in_width;
        int groups_per_output;
    } conv_params_t;

    typedef struct {
        int total_elements;
        int mismatches;
        real cosine_similarity;
        real max_diff;
        real mse;
    } verify_stats_t;

    string data_dir = "output/pe-sim/conv_k3c4";
    real verify_tolerance = 0.01;
    int clock_period_ns = DEFAULT_CLOCK_PERIOD_NS;

    logic clk;
    logic reset_n;
    logic router_enable;
    PERouterMode router_mode;
    logic pe_busy;

    noc_request_t noc_ps_in_data;
    logic noc_ps_in_valid;
    logic noc_ps_in_ready;

    noc_request_t noc_pd_in_data;
    logic noc_pd_in_valid;
    logic noc_pd_in_ready;

    noc_request_t noc_pli_in_data;
    logic noc_pli_in_valid;
    logic noc_pli_in_ready;

    noc_addr_req_t noc_plo_in_data;
    logic noc_plo_in_valid;
    logic noc_plo_in_ready;

    noc_response_t noc_plo_out_data;
    logic noc_plo_out_valid;
    logic noc_plo_out_ready;

    logic [63:0] ln_pli_data;
    logic ln_pli_valid;
    logic ln_pli_ready;
    logic [63:0] ln_plo_data;
    logic ln_plo_valid;
    logic ln_plo_ready;

    conv_params_t conv;
    logic [15:0] program[];
    logic [63:0] weights[];
    logic [15:0] activations[];
    logic [63:0] ps_inputs[];
    logic [15:0] expected_fp16[];
    logic [63:0] received_vectors[$];

    int pe_active_cycles;

    ProcessElement dut (
        .clk(clk),
        .reset_n(reset_n),
        .router_enable(router_enable),
        .router_mode(router_mode),
        .noc_ps_in_data(noc_ps_in_data),
        .noc_ps_in_valid(noc_ps_in_valid),
        .noc_ps_in_ready(noc_ps_in_ready),
        .noc_pd_in_data(noc_pd_in_data),
        .noc_pd_in_valid(noc_pd_in_valid),
        .noc_pd_in_ready(noc_pd_in_ready),
        .noc_pli_in_data(noc_pli_in_data),
        .noc_pli_in_valid(noc_pli_in_valid),
        .noc_pli_in_ready(noc_pli_in_ready),
        .noc_plo_in_data(noc_plo_in_data),
        .noc_plo_in_valid(noc_plo_in_valid),
        .noc_plo_in_ready(noc_plo_in_ready),
        .noc_plo_out_data(noc_plo_out_data),
        .noc_plo_out_valid(noc_plo_out_valid),
        .noc_plo_out_ready(noc_plo_out_ready),
        .pe_busy(pe_busy),
        .ln_pli_data(ln_pli_data),
        .ln_pli_valid(ln_pli_valid),
        .ln_pli_ready(ln_pli_ready),
        .ln_plo_data(ln_plo_data),
        .ln_plo_valid(ln_plo_valid),
        .ln_plo_ready(ln_plo_ready)
    );

    function automatic real fp16_to_real(input logic [15:0] bits);
        int exponent;
        int fraction;
        real sign_factor;
        real mantissa;
        begin
            sign_factor = bits[15] ? -1.0 : 1.0;
            exponent = bits[14:10];
            fraction = bits[9:0];

            if ((exponent == 0) && (fraction == 0)) begin
                fp16_to_real = 0.0;
            end else if (exponent == 0) begin
                mantissa = fraction / 1024.0;
                fp16_to_real = sign_factor * (2.0 ** -14.0) * mantissa;
            end else if (exponent == 31) begin
                fp16_to_real = sign_factor * 65504.0;
            end else begin
                mantissa = 1.0 + (fraction / 1024.0);
                fp16_to_real = sign_factor * (2.0 ** (exponent - 15)) * mantissa;
            end
        end
    endfunction

    task automatic read_binary_bytes(input string path, output byte unsigned data[]);
        int fd;
        int size;
        int count;
        begin
            fd = $fopen(path, "rb");
            if (fd == 0) begin
                $fatal(1, "[TB] Failed to open %s", path);
            end
            void'($fseek(fd, 0, 2));
            size = $ftell(fd);
            void'($rewind(fd));
            data = new[size];
            count = $fread(data, fd);
            $fclose(fd);
            if (count != size) begin
                $fatal(1, "[TB] Failed to read %s completely (%0d/%0d bytes)", path, count, size);
            end
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

    task automatic read_binary_u64(input string path, output logic [63:0] data[]);
        byte unsigned raw[];
        int count;
        begin
            read_binary_bytes(path, raw);
            `TB_ASSERT((raw.size() % 8) == 0, "u64 binary size must be multiple of 8")
            count = raw.size() / 8;
            data = new[count];
            for (int i = 0; i < count; i++) begin
                data[i] = {
                    raw[(i * 8) + 7], raw[(i * 8) + 6], raw[(i * 8) + 5], raw[(i * 8) + 4],
                    raw[(i * 8) + 3], raw[(i * 8) + 2], raw[(i * 8) + 1], raw[(i * 8) + 0]
                };
            end
        end
    endtask

    task automatic parse_conv_params_from_meta(input string meta_path, output conv_params_t params);
        int fd;
        int code;
        string line;
        begin
            params.kernel_size = 0;
            params.in_ch = 0;
            params.out_ch = 0;
            params.out_width = 0;
            params.in_width = 0;
            params.groups_per_output = 0;

            fd = $fopen(meta_path, "r");
            if (fd == 0) begin
                $fatal(1, "[TB] Failed to open %s", meta_path);
            end

            while (!$feof(fd)) begin
                line = "";
                code = $fgets(line, fd);
                if (code == 0) begin
                    continue;
                end
                if ($sscanf(line, "kernel_size=%d", params.kernel_size) == 1) continue;
                if ($sscanf(line, "kernel_size: %d", params.kernel_size) == 1) continue;
                if ($sscanf(line, "in_ch=%d", params.in_ch) == 1) continue;
                if ($sscanf(line, "in_ch: %d", params.in_ch) == 1) continue;
                if ($sscanf(line, "out_ch=%d", params.out_ch) == 1) continue;
                if ($sscanf(line, "out_ch: %d", params.out_ch) == 1) continue;
                if ($sscanf(line, "out_width=%d", params.out_width) == 1) continue;
                if ($sscanf(line, "out_width: %d", params.out_width) == 1) continue;
                if ($sscanf(line, "in_width=%d", params.in_width) == 1) continue;
                if ($sscanf(line, "in_width: %d", params.in_width) == 1) continue;
            end
            $fclose(fd);

            if (params.out_ch > 0) begin
                params.groups_per_output = params.out_ch / 4;
            end
        end
    endtask

    task automatic send_req(
        output noc_request_t channel_data,
        output logic channel_valid,
        input  logic channel_ready,
        input  noc_request_t req
    );
        begin
            channel_data = req;
            channel_valid = 1'b1;
            do begin
                @(posedge clk);
            end while (!channel_ready);
            channel_valid = 1'b0;
            channel_data = '0;
        end
    endtask

    task automatic send_addr_req(
        output noc_addr_req_t channel_data,
        output logic channel_valid,
        input  logic channel_ready,
        input  noc_addr_req_t req
    );
        begin
            channel_data = req;
            channel_valid = 1'b1;
            do begin
                @(posedge clk);
            end while (!channel_ready);
            channel_valid = 1'b0;
            channel_data = '0;
        end
    endtask

    task automatic wait_cycles(input int cycles);
        repeat (cycles) @(posedge clk);
    endtask

    function automatic logic [63:0] pack_fp16x4(
        input logic [15:0] values[],
        input int base_idx,
        input int lanes,
        output logic [7:0] mask_out
    );
        logic [63:0] data;
        begin
            data = 64'h0;
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

    task automatic load_test_data_conv2d;
        string inst_file;
        string weight_file;
        string activation_file;
        string ps_input_file;
        string expected_output_file;
        string meta_file;
        begin
            inst_file = {data_dir, "/pe_program.bin"};
            weight_file = {data_dir, "/weight.bin"};
            activation_file = {data_dir, "/activation_input.bin"};
            ps_input_file = {data_dir, "/ps_input.bin"};
            expected_output_file = {data_dir, "/activation_output.bin"};
            meta_file = {data_dir, "/meta.txt"};

            parse_conv_params_from_meta(meta_file, conv);
            `TB_ASSERT(conv.kernel_size > 0, "Invalid kernel_size in meta.txt")
            `TB_ASSERT(conv.in_ch > 0, "Invalid in_ch in meta.txt")
            `TB_ASSERT(conv.out_ch > 0, "Invalid out_ch in meta.txt")
            `TB_ASSERT(conv.out_width > 0, "Invalid out_width in meta.txt")
            `TB_ASSERT(conv.in_width > 0, "Invalid in_width in meta.txt")
            `TB_ASSERT(conv.groups_per_output > 0, "Invalid groups_per_output in meta.txt")

            read_binary_u16(inst_file, program);
            read_binary_u64(weight_file, weights);
            read_binary_u16(activation_file, activations);
            read_binary_u64(ps_input_file, ps_inputs);
            read_binary_u16(expected_output_file, expected_fp16);

            $display("[TB] Loaded data from %s", data_dir);
            $display("  Program: %0d instructions", program.size());
            $display("  Weights: %0d x uint64", weights.size());
            $display("  Activations: %0d x fp16", activations.size());
            $display("  PS Inputs: %0d x uint64", ps_inputs.size());
            $display("  Expected: %0d x fp16", expected_fp16.size());
            $display("[TB] Meta: kernel_size=%0d in_ch=%0d out_ch=%0d out_width=%0d in_width=%0d groups_per_output=%0d",
                conv.kernel_size, conv.in_ch, conv.out_ch, conv.out_width, conv.in_width, conv.groups_per_output);
        end
    endtask

    task automatic program_pe;
        noc_request_t req;
        logic [63:0] cmd;
        begin
            for (int i = 0; i < program.size(); i++) begin
                cmd = 64'd2;
                cmd |= (64'(i * 2) << 4);
                cmd |= (64'(program[i]) << 16);

                req.addr = 16'h0040;
                req.data = cmd;
                req.mask = '0;
                send_req(noc_ps_in_data, noc_ps_in_valid, noc_ps_in_ready, req);
            end
        end
    endtask

    task automatic start_pe;
        noc_request_t req;
        begin
            req.addr = 16'h0040;
            req.data = 64'd4;
            req.mask = '0;
            send_req(noc_ps_in_data, noc_ps_in_valid, noc_ps_in_ready, req);
            wait_cycles(5);
        end
    endtask

    task automatic ps_sender;
        noc_request_t req;
        begin
            $display("[PS] Streaming weights...");
            for (int i = 0; i < weights.size(); i++) begin
                req.addr = 16'h0000;
                req.data = weights[i];
                req.mask = '0;
                send_req(noc_ps_in_data, noc_ps_in_valid, noc_ps_in_ready, req);
                if ((i % 256) == 0) begin
                    $display("[PS] Sent weights %0d/%0d", i, weights.size());
                end
                wait_cycles(PE_GAP_CYCLES);
            end
            $display("[PS] Completed streaming weights.");
        end
    endtask

    task automatic pli_sender;
        noc_request_t req;
        int expected_vectors;
        int want_ps_inputs;
        int idx;
        begin
            expected_vectors = conv.out_width * conv.groups_per_output;
            want_ps_inputs = expected_vectors;
            if (ps_inputs.size() < want_ps_inputs) begin
                $display("[PLI] Warning: ps_inputs smaller than expected (%0d < %0d)", ps_inputs.size(), want_ps_inputs);
            end

            for (int out_pos = 0; out_pos < conv.out_width; out_pos++) begin
                for (int g = 0; g < conv.groups_per_output; g++) begin
                    idx = (out_pos * conv.groups_per_output) + g;
                    if (idx >= ps_inputs.size()) begin
                        return;
                    end
                    req.addr = 16'h0080;
                    req.data = ps_inputs[idx];
                    req.mask = '0;
                    send_req(noc_pli_in_data, noc_pli_in_valid, noc_pli_in_ready, req);
                    wait_cycles(PE_GAP_CYCLES);
                end
            end
        end
    endtask

    task automatic send_pd_position(input int position_idx);
        noc_request_t req;
        int base;
        int in_ch_step;
        int idx;
        logic [7:0] mask;
        logic [63:0] packed;
        begin
            base = position_idx * conv.in_ch;
            unique case (conv.kernel_size)
                3: in_ch_step = 4;
                5: in_ch_step = 2;
                7: in_ch_step = 1;
                default: begin
                    in_ch_step = 4;
                    $display("[PD] Warning: unexpected kernel_size=%0d", conv.kernel_size);
                end
            endcase

            for (int c = 0; c < conv.in_ch; c += in_ch_step) begin
                idx = base + c;
                if (idx >= activations.size()) begin
                    return;
                end
                packed = pack_fp16x4(activations, idx, in_ch_step, mask);
                if (mask == 0) begin
                    return;
                end
                req.addr = 16'h0040;
                req.data = packed;
                req.mask = 64'(mask);
                send_req(noc_pd_in_data, noc_pd_in_valid, noc_pd_in_ready, req);
            end
        end
    endtask

    task automatic pd_sender;
        begin
            $display("[PD] Streaming partial dot-product positions...");
            for (int in_pos = 0; in_pos < conv.in_width; in_pos++) begin
                send_pd_position(in_pos);
                wait_cycles(PE_GAP_CYCLES);
            end
            $display("[PD] Completed streaming PD positions.");
        end
    endtask

    task automatic plo_request_thread;
        noc_addr_req_t req;
        int expected_vectors;
        begin
            expected_vectors = conv.out_width * conv.groups_per_output;
            req.addr = 16'h00C0;
            for (int i = 0; i < expected_vectors; i++) begin
                send_addr_req(noc_plo_in_data, noc_plo_in_valid, noc_plo_in_ready, req);
                if (((i + 1) % 256) == 0) begin
                    $display("[PLO-REQ] Issued %0d/%0d read requests", i + 1, expected_vectors);
                end
            end
        end
    endtask

    task automatic plo_response_sink;
        int expected_vectors;
        begin
            expected_vectors = conv.out_width * conv.groups_per_output;
            noc_plo_out_ready = 1'b1;
            while (received_vectors.size() < expected_vectors) begin
                @(posedge clk);
                if (noc_plo_out_valid && noc_plo_out_ready && (noc_plo_out_data.status == NOC_OK)) begin
                    received_vectors.push_back(noc_plo_out_data.data);
                    if ((received_vectors.size() % 256) == 0) begin
                        $display("[PLO-RSP] Received %0d/%0d vectors", received_vectors.size(), expected_vectors);
                    end
                end
            end
            noc_plo_out_ready = 1'b0;
        end
    endtask

    task automatic flatten_received(output logic [15:0] received_fp16[]);
        int total;
        begin
            total = received_vectors.size() * 4;
            received_fp16 = new[total];
            for (int i = 0; i < received_vectors.size(); i++) begin
                for (int lane = 0; lane < 4; lane++) begin
                    received_fp16[(i * 4) + lane] = received_vectors[i][(lane * 16) +: 16];
                end
            end
        end
    endtask

    function automatic verify_stats_t verify_fp16_vectors(
        input logic [15:0] expected[],
        input logic [15:0] received[],
        input real tolerance
    );
        verify_stats_t stats;
        int total;
        real dot;
        real norm_expected;
        real norm_received;
        real diff;
        real expected_real;
        real received_real;
        begin
            stats.total_elements = 0;
            stats.mismatches = 0;
            stats.cosine_similarity = 0.0;
            stats.max_diff = 0.0;
            stats.mse = 0.0;
            dot = 0.0;
            norm_expected = 0.0;
            norm_received = 0.0;
            total = (expected.size() < received.size()) ? expected.size() : received.size();
            stats.total_elements = total;

            for (int i = 0; i < total; i++) begin
                expected_real = fp16_to_real(expected[i]);
                received_real = fp16_to_real(received[i]);
                diff = expected_real - received_real;
                if (diff < 0.0) diff = -diff;
                if (diff > stats.max_diff) begin
                    stats.max_diff = diff;
                end
                if (diff > tolerance) begin
                    stats.mismatches++;
                end
                stats.mse += diff * diff;
                dot += expected_real * received_real;
                norm_expected += expected_real * expected_real;
                norm_received += received_real * received_real;
            end

            if (total > 0) begin
                stats.mse = stats.mse / total;
            end
            if ((norm_expected > 0.0) && (norm_received > 0.0)) begin
                stats.cosine_similarity = dot / $sqrt(norm_expected * norm_received);
            end else begin
                stats.cosine_similarity = 1.0;
            end
            verify_fp16_vectors = stats;
        end
    endfunction

    initial begin
        void'($value$plusargs("DATA_DIR=%s", data_dir));
        void'($value$plusargs("VERIFY_TOL=%f", verify_tolerance));
        void'($value$plusargs("CLOCK_PERIOD_NS=%d", clock_period_ns));
    end

    initial begin
        clk = 1'b0;
        forever #(clock_period_ns / 2) clk = ~clk;
    end

    initial begin
        reset_n = 1'b0;
        repeat (10) @(posedge clk);
        reset_n = 1'b1;
    end

    initial begin
        noc_ps_in_data = '0;
        noc_ps_in_valid = 1'b0;
        noc_pd_in_data = '0;
        noc_pd_in_valid = 1'b0;
        noc_pli_in_data = '0;
        noc_pli_in_valid = 1'b0;
        noc_plo_in_data = '0;
        noc_plo_in_valid = 1'b0;
        noc_plo_out_ready = 1'b1;
        ln_pli_data = '0;
        ln_pli_valid = 1'b0;
        ln_plo_ready = 1'b1;
        router_enable = 1'b0;
        router_mode = PLI_FROM_BUS_PLO_TO_BUS;
        received_vectors.delete();
        pe_active_cycles = 0;

        @(posedge reset_n);

        $display("========================================");
        $display("PE Simulation Test (conv2d)");
        $display("========================================");

        router_enable = 1'b1;
        router_mode = PLI_FROM_BUS_PLO_TO_BUS;
        wait_cycles(1);

        load_test_data_conv2d();
        `TB_ASSERT(program.size() > 0, "Program must not be empty")

        $display("\n[Step 1] Programming PE...");
        program_pe();

        $display("\n[Step 2] Starting PE...");
        start_pe();

        $display("\n[Step 3] Starting traffic threads...");
        fork
            ps_sender();
            pd_sender();
            pli_sender();
            plo_request_thread();
            plo_response_sink();
        join_none

        begin
            longint unsigned waited;
            int expected_vectors;
            expected_vectors = conv.out_width * conv.groups_per_output;
            waited = 0;
            while ((received_vectors.size() < expected_vectors) && (waited < MAX_WAIT_CYCLES)) begin
                @(posedge clk);
                waited++;
                if ((waited % 20000) == 0) begin
                    $display("[TB] Waiting outputs... cycles=%0d received_vectors=%0d/%0d pe_busy=%0b",
                        waited, received_vectors.size(), expected_vectors, pe_busy);
                end
            end

            if (received_vectors.size() < expected_vectors) begin
                $display("[TB] Output timeout: received_vectors=%0d expected=%0d", received_vectors.size(), expected_vectors);
                $fatal(1, "[TB] PE output timeout");
            end else begin
                $display("[TB] Outputs collected: %0d vectors", received_vectors.size());
            end
        end

        $display("\n[Step 4] Waiting for PE halt...");
        begin
            int halt_waited;
            halt_waited = 0;
            while (pe_busy && (halt_waited < 200)) begin
                @(posedge clk);
                halt_waited++;
            end
            $display("[TB] PE halted=%s after %0d cycles", pe_busy ? "No" : "Yes", halt_waited);
        end

        $display("\n[Step 5] Verifying outputs...");
        begin
            logic [15:0] received_fp16[];
            verify_stats_t stats;
            flatten_received(received_fp16);

            $display("Expected fp16: %0d", expected_fp16.size());
            $display("Received fp16: %0d", received_fp16.size());

            stats = verify_fp16_vectors(expected_fp16, received_fp16, verify_tolerance);
            $display("----------------------------------------");
            $display("Verification Results:");
            $display("  Total Elements: %0d", stats.total_elements);
            $display("  Mismatches: %0d (Tolerance: %f)", stats.mismatches, verify_tolerance);
            $display("  Cosine Similarity: %f", stats.cosine_similarity);
            $display("  Max Difference: %e", stats.max_diff);
            $display("  MSE: %e", stats.mse);
            $display("----------------------------------------");

            if ((stats.cosine_similarity > 0.99) && (stats.mismatches == 0)) begin
                $display("PASS: tb_pe_sim");
            end else begin
                $display("FAIL: tb_pe_sim");
                $fatal(1, "[TB] Verification failed");
            end
        end

        $display("\n[Step 6] Performance Metrics:");
        $display("  Active Cycles: %0d", pe_active_cycles);
        $display("  Instructions: N/A (not exposed by RTL interface)");

        $display("\n========================================");
        $display("Test completed.");
        $display("========================================");
        $finish;
    end

    always @(posedge clk) begin
        if (!reset_n) begin
            pe_active_cycles <= 0;
        end else if (pe_busy) begin
            pe_active_cycles <= pe_active_cycles + 1;
        end
    end
endmodule