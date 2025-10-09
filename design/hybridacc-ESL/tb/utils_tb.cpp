#include "hybridacc/utils.hpp"
#include <systemc.h>
#include <iostream>

// FIFO with Ready/Valid handshake
// Testbench for utility components

SC_MODULE(AddOne) {
    sc_in<bool> clk;
    sc_in<bool> rst_n;

    // Example ports (can be expanded as needed)
    sc_port< hybridacc::ReadyValidChannel< sc_uint<32> > > input_channel{"input_channel"};
    sc_port< hybridacc::ReadyValidChannel< sc_uint<32> > > output_channel{"output_channel"};

    SC_CTOR(AddOne) {
        SC_THREAD(process);
        sensitive << clk.pos();
        async_reset_signal_is(rst_n, false);
    }

    void process() {
        while (true) {
            wait(); // Wait for clock edge (static sensitivity provides posedge wakeup)
            if (!rst_n.read()) {
                continue; // stay in reset
            }
            sc_uint<32> data;
            if (input_channel->receive(data)) {
                sc_uint<32> result = data + 1;
                wait(clk.posedge_event()); // Ensure synchronization before sending
                output_channel->send(result);
            }
        }
    }

    void initialize() {}
    void execute() {}
};

SC_MODULE(AddOneTB) {
    AddOne* addone;
    sc_clock clk{"clk", 10, SC_NS};
    sc_signal<bool> rst_n;
    hybridacc::ReadyValidChannel< sc_uint<32> > in_chan{"in_chan"};
    hybridacc::ReadyValidChannel< sc_uint<32> > out_chan{"out_chan"};

    SC_CTOR(AddOneTB) {
        in_chan.clk(clk);
        out_chan.clk(clk);
        addone = new AddOne("AddOne");
        addone->clk(clk);
        addone->rst_n(rst_n);
        addone->input_channel(in_chan);
        addone->output_channel(out_chan);

        SC_THREAD(run_test); // 移除對 sc_clock 的錯誤敏感度設定
    }

    void run_test() {
        // 同步 reset：保持 3 個時脈週期
        rst_n.write(false);
        for (int i = 0; i < 3; ++i) wait(clk.posedge_event());
        rst_n.write(true);
        wait(clk.posedge_event());

        addone->initialize();
        addone->execute();

        sc_uint<32> test_data = 5;
        in_chan.send(test_data);

        for(int i=0;i<3;i++) {
            wait(clk.posedge_event());
        }

        sc_uint<32> result; bool got = false;
        while (!got) {
            if (out_chan.receive(result)) got = true;
            if (!got) wait(clk.posedge_event());
        }

        assert(result == test_data + 1);
        std::cout << "Received result: " << result << std::endl;
        wait(clk.posedge_event());
        sc_stop();
    }

    ~AddOneTB() { delete addone; }
};

int sc_main(int argc, char* argv[]) {
    AddOneTB tb("tb");

    // 建立 VCD 波形檔
    sc_trace_file* tf = sc_create_vcd_trace_file("addone"); // 會輸出 addone.vcd
    sc_trace(tf, tb.clk, "clk");
    sc_trace(tf, tb.rst_n, "rst_n");
    sc_trace(tf, tb.addone->input_channel->ready, "in_ready");
    sc_trace(tf, tb.addone->input_channel->valid, "in_valid");
    sc_trace(tf, tb.addone->input_channel->data, "in_data");
    sc_trace(tf, tb.addone->output_channel->ready, "out_ready");
    sc_trace(tf, tb.addone->output_channel->valid, "out_valid");
    sc_trace(tf, tb.addone->output_channel->data, "out_data");

    // 若支援 FST，可額外: // sc_create_fst_trace_file("addone_fst");

    sc_start();

    std::cout << "Test completed successfully." << std::endl;

    sc_close_vcd_trace_file(tf);
    return 0;
}