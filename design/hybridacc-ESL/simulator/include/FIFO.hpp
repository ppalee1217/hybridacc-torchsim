#pragma once

#include <systemc>
#include <cassert>
#include <vector>
#include <string>
#include "utils.hpp"

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {
namespace pe {

// ============================================================================
// FIFO Module with Template Support (Pure Port-Based Interface)
// ============================================================================
// Features:
// - Template-based data type
// - Configurable depth
// - Support for simultaneous push and pop in the same cycle
// - Pure port-based interface (no methods)
// ============================================================================

template<typename T>
SC_MODULE(FIFO) {
public:
    // ============================================================================
    // Ports
    // ============================================================================
    sc_in<bool> clk;
    sc_in<bool> reset_n;

    // Write interface
    sc_in<T> data_in;
    sc_in<bool> push;

    // Read interface
    sc_out<T> data_out;
    sc_in<bool> pop;

    // Status
    sc_out<bool> empty;
    sc_out<bool> full;

    // Constructor with custom depth
    FIFO(sc_module_name name, int depth)
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
          fifo_name(name)  // Store the name
    {
        assert(fifo_depth > 0 && "FIFO depth must be > 0");

        // Initialize storage
        storage.resize(fifo_depth);
        for (int i = 0; i < fifo_depth; i++) {
            storage[i] = T();
        }

        //  使用 SC_HAS_PROCESS 來啟用 process 註冊
        SC_HAS_PROCESS(FIFO);

        SC_CTHREAD(sequential_process, clk.pos());
        reset_signal_is(reset_n, false);

        SC_METHOD(combinational_process);
        sensitive << write_ptr_reg << read_ptr_reg << count_reg << data_in << push << pop;
    }

    friend std::ostream& operator<<(std::ostream& os, const FIFO<T>& fifo) {
        os << "[FIFO: " << fifo.fifo_name << "] Depth: " << fifo.fifo_depth
           << ", Count: " << fifo.count_reg.read()
           << ", Empty: " << fifo.empty.read()
           << ", Full: " << fifo.full.read()
           << " Datastorage: [" << std::hex;
        for (int i = 0; i < fifo.fifo_depth; ++i) {
            os << fifo.storage[i];
            if (i != fifo.fifo_depth - 1) os << ", ";
        }
        os << std::dec << "]";
        return os;
    }

private:
    // Parameters
    const int fifo_depth;
    const std::string fifo_name;  // Add name storage

    // Internal storage
    std::vector<T> storage;

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
        const T head_value = (cnt > 0) ? storage[rd_ptr] : T();
        data_out.write(head_value);
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
        for (int i = 0; i < fifo_depth; i++) {
            storage[i] = T();
        }

        wait();

        while (true) {
            const int wr_ptr = write_ptr_reg.read();
            const int rd_ptr = read_ptr_reg.read();
            const int cnt = count_reg.read();

            const bool want_push = push.read();
            const bool want_pop = pop.read();

            const bool is_empty = (cnt == 0);
            const bool is_full = (cnt == fifo_depth);

            // Decide operations (synchronous semantics)
            bool do_pop = false;
            bool do_push = false;

            if (is_full) {
                do_pop = want_pop;
                do_push = want_push && want_pop; // allow push+pop when full
            } else if (is_empty) {
                do_pop = false; // no bypass on empty
                do_push = want_push;
            } else {
                do_pop = want_pop;
                do_push = want_push;
            }

            int next_wr_ptr = wr_ptr;
            int next_rd_ptr = rd_ptr;
            int next_cnt = cnt;

            if (do_push) {
                storage[wr_ptr] = data_in.read();
                next_wr_ptr = (wr_ptr + 1) % fifo_depth;
                next_cnt += 1;
            }
            if (do_pop) {
                next_rd_ptr = (rd_ptr + 1) % fifo_depth;
                next_cnt -= 1;
            }

            write_ptr_reg.write(next_wr_ptr);
            read_ptr_reg.write(next_rd_ptr);
            count_reg.write(next_cnt);

            const bool next_empty = (next_cnt == 0);
            const bool next_full = (next_cnt == fifo_depth);
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
