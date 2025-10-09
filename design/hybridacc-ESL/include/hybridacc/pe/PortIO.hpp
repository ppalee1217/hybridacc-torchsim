#ifndef HYBRIDACC_PE_PORTIO_HPP
#define HYBRIDACC_PE_PORTIO_HPP

// ============================================================================
//  File        : PortIO.hpp
//  Description : PortIO module: Implements Ready/Valid FIFO (small capacity)
//                to isolate/decouple the four channels (PLI, PLO, PS, PD)
//                between the internal and external parts of the PE.
//                - External -> Internal: PLI / PS / PD directions (inbound)
//                - Internal -> External: PLO direction (outbound)
//                Each channel uses ReadyValidFIFO to implement single-clock,
//                synchronous reset (active-low) pipelined buffering, avoiding
//                direct timing/backpressure impact from external sources.
//  Change Log  : 2025-10-05  Initial draft (copied skeleton)
//                2025-10-06  Updated file name/description, added copy prevention,
//                            unified FIFO_DEPTH constant, and cleaned up comments.
//                2025-10-06  Parameterized FIFO depth (template), added statistics
//                            and trace interfaces.
// ============================================================================

#include <systemc.h>
#include <hybridacc/utils.hpp>

using namespace sc_core;

namespace hybridacc {
namespace pe {

template<int FIFO_DEPTH = 4>
SC_MODULE(PortIO) {
    static_assert(FIFO_DEPTH > 0, "FIFO_DEPTH 必須 > 0");

    // Ports
    sc_in<bool> clk;      // Clock
    sc_in<bool> rst_n;    // Active-low synchronous reset (假設 FIFO 亦為 active-low)

    // ===================== 外部 (Outside of PE) =====================
    // PLI: 外部 -> 內部
    sc_in<sc_uint<PORT_LOCAL_WIDTH>> pli_i;
    sc_in<bool> pli_valid_i;
    sc_out<bool> pli_ready_o;

    // PLO: 內部 -> 外部
    sc_out<sc_uint<PORT_LOCAL_WIDTH>> plo_o;
    sc_out<bool> plo_valid_o;
    sc_in<bool> plo_ready_i;

    // PS: 外部 -> 內部
    sc_in<sc_uint<PORT_STATIC_WIDTH>> ps_i;
    sc_in<bool> ps_valid_i;
    sc_out<bool> ps_ready_o;

    // PD: 外部 -> 內部
    sc_in<sc_uint<PORT_DYNAMIC_WIDTH>> pd_i;
    sc_in<bool> pd_valid_i;
    sc_out<bool> pd_ready_o;

    // ===================== 內部 (Inside of PE) =====================
    // PLI: 外部 -> 內部 (此端為輸出給內部)
    sc_out<sc_uint<PORT_LOCAL_WIDTH>> pli_o;
    sc_out<bool> pli_valid_o;
    sc_in<bool> pli_ready_i;

    // PLO: 內部 -> 外部 (此端為輸入自內部)
    sc_in<sc_uint<PORT_LOCAL_WIDTH>> plo_i;
    sc_in<bool> plo_valid_i;
    sc_out<bool> plo_ready_o;

    // PS: 外部 -> 內部 (此端為輸出給內部)
    sc_out<sc_uint<PORT_STATIC_WIDTH>> ps_o;
    sc_out<bool> ps_valid_o;
    sc_in<bool> ps_ready_i;

    // PD: 外部 -> 內部 (此端為輸出給內部)
    sc_out<sc_uint<PORT_DYNAMIC_WIDTH>> pd_o;
    sc_out<bool> pd_valid_o;
    sc_in<bool> pd_ready_i;

    // ===================== 統計 (Statistics Signals) =====================
    // enqueue: 外部進入 或 (PLO) 內部進入 FIFO
    // dequeue: 從 FIFO 取出到另一端
    sc_signal<sc_uint<32>> pli_enq_cnt, pli_deq_cnt;
    sc_signal<sc_uint<32>> plo_enq_cnt, plo_deq_cnt;
    sc_signal<sc_uint<32>> ps_enq_cnt,  ps_deq_cnt;
    sc_signal<sc_uint<32>> pd_enq_cnt,  pd_deq_cnt;

    // 僅保留最大佔用 (max occupancy)，即時佔用直接 query fifo.occupancy()
    sc_signal<sc_uint<16>> pli_occupancy_max;
    sc_signal<sc_uint<16>> plo_occupancy_max;
    sc_signal<sc_uint<16>> ps_occupancy_max;
    sc_signal<sc_uint<16>> pd_occupancy_max;

    SC_CTOR(PortIO) {
        // FIFO reset / clock
        pli_fifo.clk(clk); pli_fifo.rst_n(rst_n);
        plo_fifo.clk(clk); plo_fifo.rst_n(rst_n);
        ps_fifo.clk(clk);  ps_fifo.rst_n(rst_n);
        pd_fifo.clk(clk);  pd_fifo.rst_n(rst_n);

        // Outside wiring
        pli_fifo.data_in(pli_i); pli_fifo.valid_in(pli_valid_i); pli_fifo.ready_out(pli_ready_o);
        plo_fifo.data_out(plo_o); plo_fifo.valid_out(plo_valid_o); plo_fifo.ready_in(plo_ready_i);
        ps_fifo.data_in(ps_i);  ps_fifo.valid_in(ps_valid_i);  ps_fifo.ready_out(ps_ready_o);
        pd_fifo.data_in(pd_i);  pd_fifo.valid_in(pd_valid_i);  pd_fifo.ready_out(pd_ready_o);

        // Inside wiring
        pli_fifo.data_out(pli_o); pli_fifo.valid_out(pli_valid_o); pli_fifo.ready_in(pli_ready_i);
        plo_fifo.data_in(plo_i);  plo_fifo.valid_in(plo_valid_i);  plo_fifo.ready_out(plo_ready_o);
        ps_fifo.data_out(ps_o);  ps_fifo.valid_out(ps_valid_o);  ps_fifo.ready_in(ps_ready_i);
        pd_fifo.data_out(pd_o);  pd_fifo.valid_out(pd_valid_o);  pd_fifo.ready_in(pd_ready_i);

        SC_METHOD(stats_proc);
        sensitive << clk.pos();
    }

    ~PortIO() = default;

    // 防拷貝 / 防移動（SystemC 模組不允許複製/移動）
    PortIO(const PortIO&) = delete;
    PortIO& operator=(const PortIO&) = delete;
    PortIO(PortIO&&) = delete;
    PortIO& operator=(PortIO&&) = delete;

    // 提供統計快照（非 SystemC 物件，直接讀取 sc_signal value）
    struct Stats {
        uint32_t pli_enq, pli_deq, plo_enq, plo_deq;
        uint32_t ps_enq,  ps_deq,  pd_enq,  pd_deq;
        uint16_t pli_occ, pli_occ_max, plo_occ, plo_occ_max;
        uint16_t ps_occ,  ps_occ_max,  pd_occ,  pd_occ_max;
    };

    Stats get_stats() const {
        return {
            pli_enq_cnt.read().to_uint(), pli_deq_cnt.read().to_uint(),
            plo_enq_cnt.read().to_uint(), plo_deq_cnt.read().to_uint(),
            ps_enq_cnt.read().to_uint(),  ps_deq_cnt.read().to_uint(),
            pd_enq_cnt.read().to_uint(),  pd_deq_cnt.read().to_uint(),
            (uint16_t)pli_fifo.occupancy(), (uint16_t)pli_occupancy_max.read().to_uint(),
            (uint16_t)plo_fifo.occupancy(), (uint16_t)plo_occupancy_max.read().to_uint(),
            (uint16_t)ps_fifo.occupancy(),  (uint16_t)ps_occupancy_max.read().to_uint(),
            (uint16_t)pd_fifo.occupancy(),  (uint16_t)pd_occupancy_max.read().to_uint()
        };
    }

    // 追蹤 (trace) 介面：呼叫者於 sc_main 中提供 sc_trace_file*
    void trace(sc_trace_file* tf, const std::string& prefix = "") {
        if(!tf) return;
        auto p = prefix.empty() ? std::string(name()) : prefix;
        // Ports
        sc_trace(tf, clk,        p+".clk");
        sc_trace(tf, rst_n,      p+".rst_n");
        sc_trace(tf, pli_i,      p+".pli_i");
        sc_trace(tf, pli_valid_i,p+".pli_valid_i");
        sc_trace(tf, pli_ready_o,p+".pli_ready_o");
        sc_trace(tf, pli_o,      p+".pli_o");
        sc_trace(tf, pli_valid_o,p+".pli_valid_o");
        sc_trace(tf, pli_ready_i,p+".pli_ready_i");
        sc_trace(tf, plo_i,      p+".plo_i");
        sc_trace(tf, plo_valid_i,p+".plo_valid_i");
        sc_trace(tf, plo_ready_o,p+".plo_ready_o");
        sc_trace(tf, plo_o,      p+".plo_o");
        sc_trace(tf, plo_valid_o,p+".plo_valid_o");
        sc_trace(tf, plo_ready_i,p+".plo_ready_i");
        sc_trace(tf, ps_i,       p+".ps_i");
        sc_trace(tf, ps_valid_i, p+".ps_valid_i");
        sc_trace(tf, ps_ready_o, p+".ps_ready_o");
        sc_trace(tf, ps_o,       p+".ps_o");
        sc_trace(tf, ps_valid_o, p+".ps_valid_o");
        sc_trace(tf, ps_ready_i, p+".ps_ready_i");
        sc_trace(tf, pd_i,       p+".pd_i");
        sc_trace(tf, pd_valid_i, p+".pd_valid_i");
        sc_trace(tf, pd_ready_o, p+".pd_ready_o");
        sc_trace(tf, pd_o,       p+".pd_o");
        sc_trace(tf, pd_valid_o, p+".pd_valid_o");
        sc_trace(tf, pd_ready_i, p+".pd_ready_i");
        sc_trace(tf, pli_enq_cnt,       p+".pli_enq_cnt");
        sc_trace(tf, pli_deq_cnt,       p+".pli_deq_cnt");
        sc_trace(tf, plo_enq_cnt,       p+".plo_enq_cnt");
        sc_trace(tf, plo_deq_cnt,       p+".plo_deq_cnt");
        sc_trace(tf, ps_enq_cnt,        p+".ps_enq_cnt");
        sc_trace(tf, ps_deq_cnt,        p+".ps_deq_cnt");
        sc_trace(tf, pd_enq_cnt,        p+".pd_enq_cnt");
        sc_trace(tf, pd_deq_cnt,        p+".pd_deq_cnt");
        // 由於即時 occupancy 改用查詢，不直接 trace；若需 trace 可改 expose count
        sc_trace(tf, pli_occupancy_max, p+".pli_occ_max");
        sc_trace(tf, plo_occupancy_max, p+".plo_occ_max");
        sc_trace(tf, ps_occupancy_max,  p+".ps_occ_max");
        sc_trace(tf, pd_occupancy_max,  p+".pd_occ_max");
    }

    void stats_proc() {
        if(!rst_n.read()) {
            // reset
            pli_enq_cnt = 0; pli_deq_cnt = 0; pli_occupancy_max = 0;
            plo_enq_cnt = 0; plo_deq_cnt = 0; plo_occupancy_max = 0;
            ps_enq_cnt  = 0; ps_deq_cnt  = 0; ps_occupancy_max  = 0;
            pd_enq_cnt  = 0; pd_deq_cnt  = 0; pd_occupancy_max  = 0;
            return;
        }
        bool pli_enq = pli_valid_i.read() && pli_ready_o.read();
        bool pli_deq = pli_valid_o.read() && pli_ready_i.read();
        bool plo_enq = plo_valid_i.read() && plo_ready_o.read();
        bool plo_deq = plo_valid_o.read() && plo_ready_i.read();
        bool ps_enq  = ps_valid_i.read()  && ps_ready_o.read();
        bool ps_deq  = ps_valid_o.read()  && ps_ready_i.read();
        bool pd_enq  = pd_valid_i.read()  && pd_ready_o.read();
        bool pd_deq  = pd_valid_o.read()  && pd_ready_i.read();

        update_counter(pli_enq_cnt, pli_enq);
        update_counter(pli_deq_cnt, pli_deq);
        update_counter(plo_enq_cnt, plo_enq);
        update_counter(plo_deq_cnt, plo_deq);
        update_counter(ps_enq_cnt,  ps_enq);
        update_counter(ps_deq_cnt,  ps_deq);
        update_counter(pd_enq_cnt,  pd_enq);
        update_counter(pd_deq_cnt,  pd_deq);

        // 更新 max occupancy (直接查詢 FIFO 內部 count)
        upd_max(pli_occupancy_max, pli_fifo.occupancy());
        upd_max(plo_occupancy_max, plo_fifo.occupancy());
        upd_max(ps_occupancy_max,  ps_fifo.occupancy());
        upd_max(pd_occupancy_max,  pd_fifo.occupancy());
    }

    void upd_max(sc_signal<sc_uint<16>>& occ_max, unsigned cur) {
        if(cur > occ_max.read().to_uint()) occ_max = cur;
    }
};

} // namespace pe
} // namespace hybridacc

#endif // HYBRIDACC_PE_PORTIO_HPP