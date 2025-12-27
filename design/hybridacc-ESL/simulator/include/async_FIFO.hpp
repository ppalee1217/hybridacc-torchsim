#pragma once

#include <systemc>
#include <vector>
#include "utils.hpp"
#include <cassert>

namespace hybridacc {
namespace pe {

// ============================================================================
// asyncFIFO Module with Template Support for Different Input and Output Types
// ============================================================================
// Features:
// - Template-based data types for input and output
// - Configurable depth
// - Support for simultaneous push and pop in the same cycle
// - Handles N-byte input and M-byte output with N >= M and N % M == 0
// ============================================================================

template<typename IN_T, typename OUT_T>
SC_MODULE(asyncFIFO) {
public:
    // =========================================================================
    // Ports
    // =========================================================================
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Write interface
    sc_in<IN_T> data_in;
    sc_in<size_t> mask_in; // element mask for input data
    sc_in<bool> push;

    // Read interface
    sc_out<OUT_T> data_out;
    sc_in<bool> pop;

    // Status
    sc_out<bool> empty;
    sc_out<bool> full;

    // Constructor with custom depth
    asyncFIFO(sc_module_name name, int depth)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          data_in("data_in"),
          push("push"),
          data_out("data_out"),
          pop("pop"),
          empty("empty"),
          full("full"),
          fifo_depth(depth),
          fifo_name(name),
          chunks_per_push(sizeof(IN_T) / sizeof(OUT_T))  // Calculate N/M
    {
        static_assert(sizeof(IN_T) >= sizeof(OUT_T), "Input type size must be greater than or equal to output type size.");
        static_assert(sizeof(IN_T) % sizeof(OUT_T) == 0, "Input type size must be a multiple of output type size.");

        // Initialize storage
        storage.resize(fifo_depth * chunks_per_push);
        for (int i = 0; i < fifo_depth * chunks_per_push; i++) {
            storage[i] = OUT_T();
        }

        //  使用 SC_HAS_PROCESS 來啟用 process 註冊
        SC_HAS_PROCESS(asyncFIFO);

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);

        SC_METHOD(combinational_process);
        sensitive << write_ptr_reg << read_ptr_reg << count_reg << data_in << push << pop << data_out_reg;
    }

private:
    // Parameters
    const int fifo_depth;
    const std::string fifo_name;  // Add name storage
    const int chunks_per_push;    // Number of chunks per push (N/M)

    // Internal storage
    std::vector<OUT_T> storage;

    // Registers
    sc_signal<int> write_ptr_reg;
    sc_signal<int> write_ptr_next;

    sc_signal<int> read_ptr_reg;
    sc_signal<int> read_ptr_next;

    sc_signal<int> count_reg;
    sc_signal<int> count_next;

    sc_signal<OUT_T> data_out_reg;
    sc_signal<OUT_T> data_out_next;

    // Combinational logic
    void combinational_process() {
        int wr_ptr = write_ptr_reg.read();
        int rd_ptr = read_ptr_reg.read();
        int cnt = count_reg.read();
        size_t mask = mask_in.read();

        int mask_popcount = 0;
        for (int i = 0; i < chunks_per_push; i++) {
            if ((mask >> i) & 1) mask_popcount++;
        }

        int max_elements = fifo_depth * chunks_per_push;

        bool do_push = push.read() && (cnt + mask_popcount <= max_elements);
        bool do_pop = pop.read() && (cnt > 0);

        // Calculate next state
        int next_wr_ptr = wr_ptr;
        int next_rd_ptr = rd_ptr;
        int next_cnt = cnt;
        OUT_T next_data_out = data_out_reg.read();

        // Update pointers and count based on operations
        if (do_push) {
            // Push chunks based on mask
            for (int i = 0; i < chunks_per_push; i++) {
                if ((mask >> i) & 1) {
                    storage[next_wr_ptr] = reinterpret_cast<const OUT_T*>(&data_in.read())[i];
                    next_wr_ptr = (next_wr_ptr + 1) % max_elements;
                }
            }
            next_cnt += mask_popcount;
        }

        if (do_pop) {
            // Pop one chunk at a time
            next_data_out = storage[rd_ptr];
            next_rd_ptr = (rd_ptr + 1) % max_elements;
            next_cnt -= 1;
        }

        // Write next values
        write_ptr_next.write(next_wr_ptr);
        read_ptr_next.write(next_rd_ptr);
        count_next.write(next_cnt);
        data_out_next.write(next_data_out);

        // Output status signals (combinational)
        empty.write(cnt == 0);
        // Full if we cannot guarantee a full push (conservative)
        full.write(cnt > max_elements - chunks_per_push);
        data_out.write(next_data_out);
    }

    // Sequential logic
    void sequential_process() {
        // Reset
        write_ptr_reg.write(0);
        read_ptr_reg.write(0);
        count_reg.write(0);
        data_out_reg.write(OUT_T());

        // Initialize storage
        for (int i = 0; i < fifo_depth * chunks_per_push; i++) {
            storage[i] = OUT_T();
        }

        wait();

        while (true) {
            // Update registers
            write_ptr_reg.write(write_ptr_next.read());
            read_ptr_reg.write(read_ptr_next.read());
            count_reg.write(count_next.read());
            data_out_reg.write(data_out_next.read());

            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc
