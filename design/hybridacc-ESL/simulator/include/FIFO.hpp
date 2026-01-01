#pragma once

#include <systemc>
#include <vector>
#include "utils.hpp"

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
        sensitive << write_ptr_reg << read_ptr_reg << count_reg << data_in << push << pop << data_out_reg;
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
    sc_signal<int> write_ptr_next;

    sc_signal<int> read_ptr_reg;
    sc_signal<int> read_ptr_next;

    sc_signal<int> count_reg;
    sc_signal<int> count_next;

    sc_signal<T> data_out_reg;
    sc_signal<T> data_out_next;

    // Combinational logic
    void combinational_process() {
        int wr_ptr = write_ptr_reg.read();
        int rd_ptr = read_ptr_reg.read();
        int cnt = count_reg.read();

        bool do_push = push.read() && (cnt < fifo_depth);
        bool do_pop = pop.read() && (cnt > 0);

        // Calculate next state
        int next_wr_ptr = wr_ptr;
        int next_rd_ptr = rd_ptr;
        int next_cnt = cnt;
        T next_data_out = data_out_reg.read();

        // Update pointers and count based on operations
        if (do_push && do_pop) {
            // Both push and pop in same cycle
            next_wr_ptr = (wr_ptr + 1) % fifo_depth;
            next_rd_ptr = (rd_ptr + 1) % fifo_depth;
            // Count stays the same
            if (cnt > 0) {
                next_data_out = storage[rd_ptr];
            }
        } else if (do_push) {
            // Only push
            next_wr_ptr = (wr_ptr + 1) % fifo_depth;
            next_cnt = cnt + 1;
        } else if (do_pop) {
            // Only pop
            next_rd_ptr = (rd_ptr + 1) % fifo_depth;
            next_cnt = cnt - 1;
            if (cnt > 0) {
                next_data_out = storage[rd_ptr];
            }
        }

        // Always output the data at read pointer (even if not popping)
        if (!do_pop && cnt > 0) {
            next_data_out = storage[rd_ptr];
        }

        // Write next values
        write_ptr_next.write(next_wr_ptr);
        read_ptr_next.write(next_rd_ptr);
        count_next.write(next_cnt);
        data_out_next.write(next_data_out);

        // Output status signals (combinational)
        empty.write(cnt == 0);
        full.write(cnt >= fifo_depth);
        data_out.write(next_data_out);
    }

    // Sequential logic
    void sequential_process() {
        // Reset
        write_ptr_reg.write(0);
        read_ptr_reg.write(0);
        count_reg.write(0);
        data_out_reg.write(T());

        // Initialize storage
        for (int i = 0; i < fifo_depth; i++) {
            storage[i] = T();
        }

        wait();

        while (true) {
            // Update registers
            write_ptr_reg.write(write_ptr_next.read());
            read_ptr_reg.write(read_ptr_next.read());
            count_reg.write(count_next.read());
            data_out_reg.write(data_out_next.read());

            // Handle push operation (update storage)
            if (push.read() && count_reg.read() < fifo_depth) {
                int wr_ptr = write_ptr_reg.read();
                storage[wr_ptr] = data_in.read();
            }

            // Handle pop operation (just logging)
            if (pop.read() && count_reg.read() > 0) {
                int rd_ptr = read_ptr_reg.read();
            }

            wait();
        }
    }
};

} // namespace pe
} // namespace hybridacc
