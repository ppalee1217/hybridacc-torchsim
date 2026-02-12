#pragma once

#include <systemc>
#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>
#include "utils.hpp"
#include <cassert>

using namespace sc_core;  // Add this to use SystemC types without prefix

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

    // Read interface (set pop: pop sizeof(IN_T) at once)
    sc_out<IN_T> data_out_set;
    sc_in<bool> pop_set;
    sc_out<bool> set_valid;

    // Status
    sc_out<bool> empty;
    sc_out<bool> full;

    // Constructor with custom depth
    asyncFIFO(sc_module_name name, int depth)
        : sc_module(name),
          clk("clk"),
          reset_n("reset_n"),
          data_in("data_in"),
                    mask_in("mask_in"),
          push("push"),
          data_out("data_out"),
          pop("pop"),
          data_out_set("data_out_set"),
          pop_set("pop_set"),
          set_valid("set_valid"),
          empty("empty"),
          full("full"),
          fifo_depth(depth),
          fifo_name(name),
          chunks_per_push(sizeof(IN_T) / sizeof(OUT_T))  // Calculate N/M
    {
        assert(fifo_depth > 0 && "FIFO depth must be > 0");
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
        sensitive << read_ptr_reg << count_reg;
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
    sc_signal<int> read_ptr_reg;
    sc_signal<int> count_reg;
    sc_signal<bool> empty_reg;
    sc_signal<bool> full_reg;

    // Combinational logic (purely for data_out)
    void combinational_process() {
        const int rd_ptr = read_ptr_reg.read();
        const int cnt = count_reg.read();
        const OUT_T head_value = (cnt > 0) ? storage[rd_ptr] : OUT_T();
        data_out.write(head_value);

        const bool has_set = (cnt >= chunks_per_push);
        set_valid.write(has_set);

        IN_T set_value{};
        if (has_set) {
            if constexpr (std::is_trivially_copyable_v<IN_T> && std::is_trivially_copyable_v<OUT_T>) {
                std::array<std::byte, sizeof(IN_T)> buf{};
                for (int i = 0; i < chunks_per_push; i++) {
                    const int idx = (rd_ptr + i) % (fifo_depth * chunks_per_push);
                    std::memcpy(buf.data() + (static_cast<size_t>(i) * sizeof(OUT_T)),
                                &storage[idx], sizeof(OUT_T));
                }
                std::memcpy(&set_value, buf.data(), sizeof(IN_T));
            } else {
                OUT_T* chunks = reinterpret_cast<OUT_T*>(&set_value);
                for (int i = 0; i < chunks_per_push; i++) {
                    const int idx = (rd_ptr + i) % (fifo_depth * chunks_per_push);
                    chunks[i] = storage[idx];
                }
            }
        }
        data_out_set.write(set_value);
    }

    // Sequential logic
    void sequential_process() {
        // Reset
        write_ptr_reg.write(0);
        read_ptr_reg.write(0);
        count_reg.write(0);
        empty_reg.write(true);
        full_reg.write(false);
        empty.write(true);
        full.write(false);

        // Initialize storage
        for (int i = 0; i < fifo_depth * chunks_per_push; i++) {
            storage[i] = OUT_T();
        }

        wait();

        while (true) {
            const int wr_ptr = write_ptr_reg.read();
            const int rd_ptr = read_ptr_reg.read();
            const int cnt = count_reg.read();

            const bool want_push = push.read();
            const bool want_pop = pop.read();
            const bool want_pop_set = pop_set.read();

            const size_t mask = mask_in.read();
            int mask_popcount = 0;
            for (int i = 0; i < chunks_per_push; i++) {
                if ((mask >> i) & 1U) {
                    mask_popcount++;
                }
            }

            const int max_elements = fifo_depth * chunks_per_push;
            const bool is_empty = (cnt == 0);

            bool do_pop = false;
            bool do_pop_set = false;
            bool do_push = false;

            if (want_pop_set && (cnt >= chunks_per_push)) {
                do_pop_set = true;
            } else if (!is_empty && want_pop) {
                do_pop = true;
            }

            const int remaining_after_pop = cnt - (do_pop ? 1 : 0) - (do_pop_set ? chunks_per_push : 0);
            if (want_push && (remaining_after_pop + mask_popcount <= max_elements)) {
                do_push = true;
            }

            int next_wr_ptr = wr_ptr;
            int next_rd_ptr = rd_ptr;
            int next_cnt = cnt;

            if (do_push) {
                const IN_T in_val = data_in.read();
                if constexpr (std::is_trivially_copyable_v<IN_T> && std::is_trivially_copyable_v<OUT_T>) {
                    std::array<std::byte, sizeof(IN_T)> buf{};
                    std::memcpy(buf.data(), &in_val, sizeof(IN_T));
                    for (int i = 0; i < chunks_per_push; i++) {
                        if ((mask >> i) & 1U) {
                            OUT_T chunk{};
                            std::memcpy(&chunk, buf.data() + (static_cast<size_t>(i) * sizeof(OUT_T)), sizeof(OUT_T));
                            storage[next_wr_ptr] = chunk;
                            next_wr_ptr = (next_wr_ptr + 1) % max_elements;
                        }
                    }
                } else {
                    const OUT_T* chunks = reinterpret_cast<const OUT_T*>(&in_val);
                    for (int i = 0; i < chunks_per_push; i++) {
                        if ((mask >> i) & 1U) {
                            storage[next_wr_ptr] = chunks[i];
                            next_wr_ptr = (next_wr_ptr + 1) % max_elements;
                        }
                    }
                }
                next_cnt += mask_popcount;
            }

            if (do_pop_set) {
                next_rd_ptr = (rd_ptr + chunks_per_push) % max_elements;
                next_cnt -= chunks_per_push;
            } else if (do_pop) {
                next_rd_ptr = (rd_ptr + 1) % max_elements;
                next_cnt -= 1;
            }

            write_ptr_reg.write(next_wr_ptr);
            read_ptr_reg.write(next_rd_ptr);
            count_reg.write(next_cnt);

            const bool next_empty = (next_cnt == 0);
            const bool next_full = (next_cnt > max_elements - chunks_per_push);
            empty_reg.write(next_empty);
            full_reg.write(next_full);
            empty.write(next_empty);
            full.write(next_full);

            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc
