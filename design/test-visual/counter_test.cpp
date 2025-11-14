#include "counter.h"
#include <systemc.h>
#include <iostream>

// Test bench for the Counter module
SC_MODULE(CounterTestbench) {
    // Signals to connect to the Counter
    sc_signal<bool> clk;
    sc_signal<bool> reset;
    sc_signal<bool> set;
    sc_signal<bool> clear;
    sc_signal<bool> increase;
    sc_signal<int>  set_value;
    sc_signal<int>  count_out;
    sc_signal<bool> overflow;

    // Clock generator
    sc_clock clock;

    // Counter instance
    Counter* counter_inst;

    // Constructor
    SC_CTOR(CounterTestbench) : clock("clock", 10, SC_NS) {
        // Create counter instance
        counter_inst = new Counter("counter");
        
        // Connect signals
        counter_inst->clk(clock);
        counter_inst->reset(reset);
        counter_inst->set(set);
        counter_inst->clear(clear);
        counter_inst->increase(increase);
        counter_inst->set_value(set_value);
        counter_inst->count_out(count_out);
        counter_inst->overflow(overflow);

        // Test process
        SC_THREAD(test_process);
        
        // Monitor process
        SC_THREAD(monitor_process);
        sensitive << count_out << overflow;
    }

    void test_process() {
        // Initialize all signals
        reset.write(false);
        set.write(false);
        clear.write(false);
        increase.write(false);
        set_value.write(0);
        
        wait(20, SC_NS);

        cout << "\n=== Counter Test Started ===" << endl;

        // Test 1: Reset functionality
        cout << "\nTest 1: Reset Counter" << endl;
        reset.write(true);
        wait(10, SC_NS);
        reset.write(false);
        wait(10, SC_NS);

        // Test 2: Increase functionality
        cout << "\nTest 2: Increase Counter" << endl;
        for (int i = 0; i < 5; i++) {
            increase.write(true);
            wait(10, SC_NS);
            increase.write(false);
            wait(10, SC_NS);
        }

        // Test 3: Set functionality
        cout << "\nTest 3: Set Counter to 100" << endl;
        set_value.write(100);
        set.write(true);
        wait(10, SC_NS);
        set.write(false);
        wait(10, SC_NS);

        // Test 4: Clear functionality
        cout << "\nTest 4: Clear Counter" << endl;
        clear.write(true);
        wait(10, SC_NS);
        clear.write(false);
        wait(10, SC_NS);

        // Test 5: Overflow functionality
        cout << "\nTest 5: Test Overflow" << endl;
        set_value.write(999);
        set.write(true);
        wait(10, SC_NS);
        set.write(false);
        wait(10, SC_NS);

        // Increase beyond maximum
        for (int i = 0; i < 3; i++) {
            increase.write(true);
            wait(10, SC_NS);
            increase.write(false);
            wait(10, SC_NS);
        }

        // Test 6: Set beyond maximum
        cout << "\nTest 6: Set Beyond Maximum" << endl;
        set_value.write(1500);
        set.write(true);
        wait(10, SC_NS);
        set.write(false);
        wait(10, SC_NS);

        wait(50, SC_NS);
        cout << "\n=== All Tests Completed ===" << endl;
        sc_stop();
    }

    void monitor_process() {
        while (true) {
            wait();
            cout << "Time: " << sc_time_stamp() 
                 << " | Count: " << count_out.read() 
                 << " | Overflow: " << (overflow.read() ? "YES" : "NO") << endl;
        }
    }

    ~CounterTestbench() {
        delete counter_inst;
    }
};

int sc_main(int argc, char* argv[]) {
    cout << "SystemC Counter Module Test" << endl;
    cout << "===========================" << endl;

    // Create testbench
    CounterTestbench testbench("testbench");

    // Run simulation
    sc_start();

    cout << "\nSimulation completed successfully!" << endl;
    return 0;
}