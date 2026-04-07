#pragma once

#include <systemc>
#include <vector>
#include <string>
#include "Utils/utils.hpp"

using namespace sc_core;  // Add this to use SystemC types without prefix

namespace hybridacc {

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

    // Clear
    sc_in<bool> clear;

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
          clear("clear"),
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
        sensitive  << read_ptr_reg << count_reg << write_ptr_reg;
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
        empty.write(true);
        full.write(false);

        // Initialize storage
        for (int i = 0; i < fifo_depth; i++) {
            storage[i] = T();
        }

        wait();

        while (true) {
            if (clear.read()) {
                write_ptr_reg.write(0);
                read_ptr_reg.write(0);
                count_reg.write(0);
                empty.write(true);
                full.write(false);
                wait();
                continue;
            }

            const int wr_ptr = write_ptr_reg.read();
            const int rd_ptr = read_ptr_reg.read();
            const int cnt = count_reg.read();

            const bool want_push = push.read();
            const bool want_pop = pop.read();

            const bool is_empty = (cnt <= 0);
            const bool is_full = (cnt >= fifo_depth);

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

            // pop first, then push, warning detection
            if (want_pop && is_empty) {
                SC_REPORT_ERROR(fifo_name.c_str(), "Attempting to pop from an empty FIFO. Operation ignored.");
            }
            if (want_push && is_full && !want_pop) {
                std::stringstream warning_msg;
                warning_msg << "Attempting to push into a full FIFO. Operation ignored. Data: 0x" << std::hex << data_in.read() << std::dec;
                SC_REPORT_ERROR(fifo_name.c_str(), warning_msg.str().c_str());
            }


            int next_wr_ptr = wr_ptr;
            int next_rd_ptr = rd_ptr;
            int next_cnt = cnt;
            T popped_data = T();

            // pop first, then push
            if (do_pop) {
                next_rd_ptr = (rd_ptr + 1) % fifo_depth;
                next_cnt -= 1;
                popped_data = storage[rd_ptr];
            }
            if (do_push) {
                storage[wr_ptr] = data_in.read();
                next_wr_ptr = (wr_ptr + 1) % fifo_depth;
                next_cnt += 1;
            }


            write_ptr_reg.write(next_wr_ptr);
            read_ptr_reg.write(next_rd_ptr);
            count_reg.write(next_cnt);

            const bool next_empty = (next_cnt <= 0);
            const bool next_full = (next_cnt >= fifo_depth);
            empty.write(next_empty);
            full.write(next_full);


            if(do_pop) {
                DEBUG_MSG("[" << fifo_name << "] Popped data: " << std::hex << popped_data << std::dec
                          << " from index " << rd_ptr << ", cnt " << next_cnt << ", empty " << next_empty << ", full " << next_full, DEBUG_LEVEL_PE_COMPONENTS);
            }
            if(do_push) {
                DEBUG_MSG("[" << fifo_name << "] Pushed data: " << std::hex << data_in.read() << std::dec
                          << " to index " << wr_ptr << ", cnt " << next_cnt << ", empty " << next_empty << ", full " << next_full, DEBUG_LEVEL_PE_COMPONENTS);
            }

            wait();
        }
    }
};

} // namespace hybridacc
