#ifndef HYBRIDACC_NOC_MULTICAST_HPP
#define HYBRIDACC_NOC_MULTICAST_HPP


// ============================================================================
//  File        : multicast.hpp
//  Module      : NoC::Multicast
//  Description : 定義 NoC 中的 Multicast 節點。
//                Multicast 節點會將輸入資料複製並送到多個輸出端口。
//                支援動態設定目標輸出端口 (via控制訊號)。
// ============================================================================
#include <systemc.h>
#include <vector>
#include <bitset>
#include <iostream>
#include "hybridacc/utils.hpp"   // Get DATA_WIDTH and related macros

namespace hybridacc { namespace noc {
// Element width equals DATA_WIDTH (platform defines 16 bits)
using element_t = sc_uint<DATA_WIDTH>;
using vector_t = sc_uint<DATA_WIDTH*VECTOR_SIZE>; // 4 lanes of DATA

SC_MODULE(Multicast) {
    // -------- Ports --------
    sc_in<bool> clk{"clk"};
    sc_in<bool> rst_n{"rst_n"};        // Active-low reset

    sc_in<vector_t> data_in{"data_in"};       // Input data
    sc_in<bool> valid_in{"valid_in"};         // Input valid
    sc_out<bool> ready_in{"ready_in"};        // Input ready

    static const int NUM_OUTPUTS = 4;         // Number of output ports
    sc_out<vector_t> data_out[NUM_OUTPUTS];   // Output data ports
    sc_out<bool> valid_out[NUM_OUTPUTS];      // Output valid ports
    sc_in<bool> ready_out[NUM_OUTPUTS];       // Output ready ports

    sc_in<std::bitset<NUM_OUTPUTS>> target_mask{"target_mask"}; // Target output mask

    // -------- Internal signals --------
    bool sending{false};                       // Indicates if currently sending data
    vector_t current_data;                     // Holds the current data being sent

    // -------- Processes --------
    SC_CTOR(Multicast) {
        SC_METHOD(comb_proc);
        sensitive << data_in << valid_in << target_mask;
        for (int i = 0; i < NUM_OUTPUTS; ++i) {
            sensitive << ready_out[i];
        }
        dont_initialize();

        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();
    }


}}

#endif // HYBRIDACC_NOC_MULTICAST_HPP