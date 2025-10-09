#include <systemc>
#include <iostream>
#include "hybridacc/hybridacc.hpp"
#include "hybridacc/DMA.hpp"
#include "hybridacc/noc/noc.hpp"

// 簡易單元測試：
// 1. 建立 HybridAccelerator 並初始化
// 2. 提交 DMA copy 請求模擬數個週期
// 3. 驗證請求完成

int sc_main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    sc_clock clk{"clk", 10, SC_NS};
    sc_signal<bool> rst_n{"rst_n"};

    hybridacc::HybridAccelerator accel;
    hybridacc::PlatformDesc desc{ .arrays=1, .pe_rows=1, .pe_cols=1 };
    accel.initialize(desc);

    // 建立獨立 DMA 做測試 (目前 HybridAccelerator 內部尚未連時脈, 這裡另外實例化小測試)
    hybridacc::DMA dma("dma_tb");
    dma.clk(clk);
    dma.rst_n(rst_n);

    rst_n.write(false);
    sc_start(25, SC_NS); // reset 幾拍
    rst_n.write(true);

    // 提交一個 128 bytes 搬移
    auto tag = dma.submit_copy(0x1000, 0x2000, 128);

    // 模擬直到完成
    while(!dma.poll(tag)) {
        sc_start(10, SC_NS); // 一個 clock 週期
    }

    std::cout << "DMA copy tag=" << tag << " completed at t=" << sc_time_stamp() << "\n";

    accel.report_stats(std::cout);
    sc_stop();
    return 0;
}