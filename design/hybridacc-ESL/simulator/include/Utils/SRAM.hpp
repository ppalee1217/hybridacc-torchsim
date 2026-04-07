#pragma once

#include <systemc>
#include <vector>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include "Utils/utils.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

// Configurable SRAM module  — RTL-style ESL implementation
//
// Template parameters:
//  - DATA_WIDTH_BITS : data bus width in bits (must be divisible by 8)
//  - ADDR_WIDTH      : address field width in bits
//
// Architecture overview
// ─────────────────────
//  All architectural state lives in sc_signal<T> registers (suffix _reg).
//  A single SC_CTHREAD  (seq_process)    computes and commits next-state.
//  Two SC_METHODs       (comb_req_ready, comb_resp_out) drive output ports
//  purely from register values — no combinational feedback loops.
//
//  The read pipeline is modelled as a circular FIFO of depth `pipeline_depth`:
//    pipe_valid_reg[i], pipe_addr_reg[i], pipe_cycles_reg[i]
//  tracked by head_reg (read pointer) and count_reg (occupancy).
//  An output holding register (resp_data_reg / resp_valid_reg) correctly
//  back-pressures the pipeline: the head slot is never popped while the
//  output register is stalled (resp_valid && !resp_ready).

template <unsigned DATA_WIDTH_BITS = 64, unsigned ADDR_WIDTH = 32>
SC_MODULE(SRAM) {
    static_assert(DATA_WIDTH_BITS % 8 == 0, "DATA_WIDTH_BITS must be a multiple of 8");
    static_assert(ADDR_WIDTH     <= 32,     "ADDR_WIDTH must be <= 32");

    // ── Ports ────────────────────────────────────────────────────────────────
    sc_in<bool>  clk;
    sc_in<bool>  reset_n;

    // Read request interface
    sc_in < sc_uint<ADDR_WIDTH> >        req_addr;
    sc_in <bool>                         req_valid;
    sc_out<bool>                         req_ready;

    // Read response interface
    sc_out< sc_biguint<DATA_WIDTH_BITS> > resp_data;
    sc_out<bool>                          resp_valid;
    sc_in <bool>                          resp_ready;

    // Synchronous write interface
    sc_in<bool>                           write_en;
    sc_in < sc_uint<ADDR_WIDTH> >         write_addr;
    sc_in < sc_biguint<DATA_WIDTH_BITS> > write_data;
    sc_in < sc_uint<DATA_WIDTH_BITS/8>  > write_mask; // byte-enable, 1 bit per byte

    // ── Configuration (set once at construction) ──────────────────────────────
    const size_t data_width_bytes;  // DATA_WIDTH_BITS / 8
    const size_t default_latency;   // read latency in clock cycles (>= 1)
    const size_t pipeline_depth;    // max outstanding read requests (1..255)

    // ── Internal memory ───────────────────────────────────────────────────────
    std::vector<uint8_t> mem;

    // ── Pipeline FIFO registers (circular buffer, size = pipeline_depth) ──────
    // Each slot represents one in-flight read request.
    std::vector< sc_signal<bool>              > pipe_valid_reg;  // slot occupied
    std::vector< sc_signal< sc_uint<ADDR_WIDTH> > > pipe_addr_reg;  // byte address
    std::vector< sc_signal< sc_uint<8> >      > pipe_cycles_reg; // countdown (max latency 255)

    // FIFO head pointer and occupancy
    sc_signal< sc_uint<8> > head_reg;   // index of the oldest entry
    sc_signal< sc_uint<8> > count_reg;  // number of occupied slots

    // ── Output-stage registers ────────────────────────────────────────────────
    sc_signal< sc_biguint<DATA_WIDTH_BITS> > resp_data_reg;
    sc_signal<bool>                          resp_valid_reg;

    // ─────────────────────────────────────────────────────────────────────────

    SC_HAS_PROCESS(SRAM);

    // Constructor
    // size_bytes : total byte capacity of the memory array
    // latency    : read latency in cycles (>= 1)
    // pip_depth  : maximum in-flight read requests (1..255)
    SRAM(sc_module_name name,
         size_t size_bytes = (1u << 16), // default 64 KiB
         size_t latency    = 1,
         size_t pip_depth  = 3)
        : sc_module(name),
          clk("clk"), reset_n("reset_n"),
          req_addr("req_addr"), req_valid("req_valid"), req_ready("req_ready"),
          resp_data("resp_data"), resp_valid("resp_valid"), resp_ready("resp_ready"),
          write_en("write_en"), write_addr("write_addr"),
          write_data("write_data"), write_mask("write_mask"),
          data_width_bytes(DATA_WIDTH_BITS / 8),
          default_latency(latency),
          pipeline_depth(pip_depth),
          mem(size_bytes, 0),
          pipe_valid_reg (pip_depth),
          pipe_addr_reg  (pip_depth),
          pipe_cycles_reg(pip_depth),
          head_reg  ("head_reg"),
          count_reg ("count_reg"),
          resp_data_reg ("resp_data_reg"),
          resp_valid_reg("resp_valid_reg")
    {
        // ── Register processes ────────────────────────────────────────────────
        SC_CTHREAD(seq_process, clk.pos());
        reset_signal_is(reset_n, false);

        // ── Combinational output processes ────────────────────────────────────
        SC_METHOD(comb_req_ready);
        sensitive << count_reg;

        SC_METHOD(comb_resp_out);
        sensitive << resp_valid_reg << resp_data_reg;

        SC_METHOD(trace_process);
        sensitive << clk.pos();
    }

    // ── Public utilities ──────────────────────────────────────────────────────

    // Zero the memory array (does NOT affect pipeline or output registers;
    // call only during sc_start-time setup or testbench initialisation).
    void mem_reset() { std::fill(mem.begin(), mem.end(), 0); }

    // Resize the memory array (bytes).
    void resize(size_t bytes) { mem.resize(bytes, 0); }
    size_t size_bytes_total() const { return mem.size(); }

    // Pretty-print a memory range (hex dump, for debugging).
    std::string dump(size_t start = 0, size_t len = 64) const {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < len && (start + i) < mem.size(); ++i) {
            if ((i % 16) == 0)
                oss << "\n" << std::setw(8) << (start + i) << ": ";
            oss << std::setw(2) << static_cast<unsigned>(mem[start + i]) << " ";
        }
        return oss.str();
    }

    // ── Trace helpers ────────────────────────────────────────────────────────
    void set_trace_context(uint32_t pid, int tid_base) {
        trace_pid = pid;
        trace_id = tid_base;
        trace_init = false;
        last_state = "IDLE";
    }

    int get_trace_num() const { return 1; }

private:
    // Trace context
    uint32_t trace_pid = static_cast<uint32_t>(TRACE_PID::PE_ROUTER);
    int trace_id = -1;
    bool trace_init = false;
    std::string last_state = "IDLE";

    // ── Memory helpers ────────────────────────────────────────────────────────

    sc_biguint<DATA_WIDTH_BITS> read_word(uint32_t byte_addr) const {
        sc_biguint<DATA_WIDTH_BITS> out = 0;
        const size_t base = byte_addr % mem.size();
        for (size_t i = 0; i < data_width_bytes; ++i)
            out.range(8*i + 7, 8*i) = mem[(base + i) % mem.size()];
        return out;
    }

    void write_word(uint32_t byte_addr,
                    sc_biguint<DATA_WIDTH_BITS> value,
                    uint64_t byte_mask)
    {
        const size_t base = byte_addr % mem.size();
        for (size_t i = 0; i < data_width_bytes; ++i) {
            if (byte_mask & (1ULL << i))
                mem[(base + i) % mem.size()] =
                    static_cast<uint8_t>(value.range(8*i + 7, 8*i).to_uint());
        }
    }

    // ── Combinational process: req_ready ──────────────────────────────────────
    // Driven purely from count_reg; no feedback from outputs.
    void comb_req_ready() {
        req_ready.write(count_reg.read() < static_cast<sc_uint<8>>(pipeline_depth));
    }

    // ── Combinational process: resp_valid / resp_data ─────────────────────────
    // Transparently forward the output-stage registers to port outputs.
    void comb_resp_out() {
        resp_valid.write(resp_valid_reg.read());
        resp_data .write(resp_data_reg .read());
    }

    // ── Sequential process: next-state computation ────────────────────────────
    //
    // RTL discipline:
    //   • Every .read() observes the register value from the PREVIOUS clock edge.
    //   • Every .write() schedules the value to be latched on the NEXT clock edge.
    //   • No signal is read after being written in the same "between-wait" window.
    //
    // Pipeline timing with default_latency == L:
    //   cycle 0      : request pushed, pipe_cycles_reg[slot] = L
    //   cycle 1..L-1 : countdown decrements each cycle
    //   cycle L      : cycles_left reaches 1 at start of cycle → popped this cycle
    //                  → response appears on port in cycle L  (latency = L cycles)
    void seq_process() {
        // ── Synchronous reset ─────────────────────────────────────────────────
        for (size_t i = 0; i < pipeline_depth; ++i) {
            pipe_valid_reg [i].write(false);
            pipe_addr_reg  [i].write(0);
            pipe_cycles_reg[i].write(0);
        }
        head_reg      .write(0);
        count_reg     .write(0);
        resp_data_reg .write(0);
        resp_valid_reg.write(false);
        wait();

        // ── Main loop ─────────────────────────────────────────────────────────
        while (true) {

            // Snapshot current register state (all reads reflect previous edge).
            const unsigned head  = head_reg  .read().to_uint();
            const unsigned count = count_reg .read().to_uint();
            const bool     rv    = resp_valid_reg.read(); // output stage occupied
            const bool     rr    = resp_ready    .read(); // downstream ready

            // ── Synchronous write to memory ───────────────────────────────────
            if (write_en.read())
                write_word(write_addr.read().to_uint(),
                           write_data.read(),
                           write_mask.read().to_uint64());

            // ── Decrement countdown for every occupied pipeline slot ───────────
            // Reads see current values; writes take effect on next edge.
            for (size_t i = 0; i < pipeline_depth; ++i) {
                if (pipe_valid_reg[i].read()) {
                    const sc_uint<8> c = pipe_cycles_reg[i].read();
                    if (c > 1)
                        pipe_cycles_reg[i].write(static_cast<sc_uint<8>>(c - 1));
                }
            }

            // ── Pop decision ──────────────────────────────────────────────────
            // Pop the head when:
            //   (a) FIFO is non-empty
            //   (b) its countdown is about to hit zero (cycles_left == 1
            //       before decrement → 0 after decrement = completion)
            //   (c) the output holding register is free to accept new data
            //       (!rv means free; rv && rr means downstream accepts this cycle)
            const bool head_done = (count > 0) &&
                                   (pipe_cycles_reg[head].read().to_uint() <= 1);
            const bool out_free  = !rv || rr;
            const bool do_pop    = head_done && out_free;

            if (do_pop) {
                // Latch data into output register.
                resp_data_reg .write(read_word(pipe_addr_reg[head].read().to_uint()));
                resp_valid_reg.write(true);
                // Free the head slot and advance the FIFO head.
                pipe_valid_reg[head].write(false);
                head_reg .write(static_cast<sc_uint<8>>((head + 1) % pipeline_depth));
                count_reg.write(static_cast<sc_uint<8>>(count - 1));
            } else if (rv && rr) {
                // Downstream accepted the current output; nothing new to present.
                resp_valid_reg.write(false);
            }
            // Otherwise: if (rv && !rr) the output register holds its value
            // automatically (no write needed — RTL register retains state).

            // ── Push decision ─────────────────────────────────────────────────
            // A new request can be pushed this cycle when:
            //   • req_valid is asserted, AND
            //   • the FIFO has a free slot after accounting for the pop above
            //     (effective occupancy = count - do_pop; must be < pipeline_depth).
            const unsigned effective_count = count - (do_pop ? 1u : 0u);
            const bool do_push = req_valid.read() &&
                                 (effective_count < pipeline_depth);

            if (do_push) {
                // Tail slot index relative to the current head pointer.
                // NOTE: if do_pop freed the head slot but count was already
                // computed before the pop, tail = (head + count) % N still
                // points to the correct first-free slot because the freed slot
                // is at 'head', not at 'tail'.
                const unsigned tail = (head + count) % pipeline_depth;
                pipe_valid_reg [tail].write(true);
                pipe_addr_reg  [tail].write(req_addr.read());
                pipe_cycles_reg[tail].write(
                    static_cast<sc_uint<8>>(default_latency));
                count_reg.write(static_cast<sc_uint<8>>(effective_count + 1));
            }

            wait();
        }
    }

    // ── Trace process ────────────────────────────────────────────────────────
    void trace_process() {
        if (trace_id < 0) return;

        const uint32_t tid = static_cast<uint32_t>(trace_id);
        if (!trace_init) {
            TRACE_THREAD_NAME(trace_pid, tid, std::string(name()) + " SRAM");
            TRACE_EVENT(last_state, "SRAM_State", TRACE_BEGIN, trace_pid, tid, "{}");
            trace_init = true;
        }

        const bool in_reset  = !reset_n.read();
        const bool req_v     = req_valid.read();
        const bool req_r     = req_ready.read();
        const bool req_fire  = req_v && req_r;
        const bool resp_v    = resp_valid.read();
        const bool resp_r    = resp_ready.read();
        const bool resp_fire = resp_v && resp_r;
        const bool wen   = write_en.read();

        std::string state = "IDLE";
        if (in_reset) {
            state = "RESET";
        } else if (resp_v && !resp_r) {
            state = "RESP_BACKPRESSURE";
        } else if (req_v && !req_r) {
            state = "REQ_BACKPRESSURE";
        } else if (req_fire && resp_fire) {
            state = "REQ_RESP_XFER";
        } else if (req_fire) {
            state = "REQ_XFER";
        } else if (resp_fire) {
            state = "RESP_XFER";
        } else if (count_reg.read() > 0 || resp_valid_reg.read()) {
            state = "BUSY";
        } else if (wen) {
            state = "WRITE";
        }

        if (state != last_state) {
            TRACE_EVENT(last_state, "SRAM_State", TRACE_END, trace_pid, tid, "{}");
            TRACE_EVENT(state, "SRAM_State", TRACE_BEGIN, trace_pid, tid,
                        std::string("{\"req_v\": ") + (req_v ? "true" : "false") +
                        ", \"req_r\": " + (req_r ? "true" : "false") +
                        ", \"resp_v\": " + (resp_v ? "true" : "false") +
                        ", \"resp_r\": " + (resp_r ? "true" : "false") +
                        ", \"count\": " + std::to_string(count_reg.read().to_uint()) + "}");
            last_state = state;
        }
    }

}; // SC_MODULE(SRAM)

} // namespace cluster
} // namespace hybridacc
