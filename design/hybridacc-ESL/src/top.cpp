#include <systemc>
#include <iostream>
#include "hybridacc/hybridacc.hpp"

int sc_main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    hybridacc::HybridAccelerator accel;
    hybridacc::PlatformDesc desc{ .arrays=1, .pe_rows=1, .pe_cols=2 };
    accel.initialize(desc);
    accel.run();
    // 簡化：模擬 10ns (若未實際建立 SystemC 模組執行緒也可忽略)
    sc_start(10, SC_NS);
    accel.report_stats(std::cout);
    sc_stop();
    return 0;
}
