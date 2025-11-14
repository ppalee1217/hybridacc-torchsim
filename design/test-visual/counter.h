#ifndef COUNTER_H
#define COUNTER_H

#include <systemc.h>

SC_MODULE(Counter) {
    // Ports
    sc_in<bool> clk;           // Clock signal
    sc_in<bool> reset;         // Reset signal (active high)
    sc_in<bool> set;           // Set signal - sets counter to a specific value
    sc_in<bool> clear;         // Clear signal - clears counter to 0
    sc_in<bool> increase;      // Increase signal - increments counter by 1
    sc_in<int>  set_value;     // Value to set when set signal is active
    sc_out<int> count_out;     // Counter output value
    sc_out<bool> overflow;     // Overflow flag

    // Internal variables
    int counter;
    static const int MAX_COUNT = 1000;  // Maximum counter value

    // Constructor
    SC_CTOR(Counter) {
        SC_CTHREAD(counter_process, clk.pos());
        reset_signal_is(reset, true);  // Set reset sensitivity
        counter = 0;
    }

    // Main process
    void counter_process() {
        // Reset initialization
        counter = 0;
        count_out.write(counter);
        overflow.write(false);
        wait();

        while (true) {
            // Check for reset (highest priority)
            if (reset.read()) {
                counter = 0;
                count_out.write(counter);
                overflow.write(false);
            }
            // Check for clear signal
            else if (clear.read()) {
                counter = 0;
                count_out.write(counter);
                overflow.write(false);
            }
            // Check for set signal
            else if (set.read()) {
                counter = set_value.read();
                if (counter > MAX_COUNT) {
                    counter = MAX_COUNT;
                    overflow.write(true);
                } else {
                    overflow.write(false);
                }
                count_out.write(counter);
            }
            // Check for increase signal
            else if (increase.read()) {
                if (counter < MAX_COUNT) {
                    counter++;
                    overflow.write(false);
                } else {
                    overflow.write(true);  // Set overflow flag if at maximum
                }
                count_out.write(counter);
            }
            
            wait();  // Wait for next clock edge
        }
    }

    // Method to get current counter value (for debugging)
    int get_count() const {
        return counter;
    }

    // Method to check if counter is at maximum
    bool is_at_max() const {
        return counter >= MAX_COUNT;
    }
};

#endif // COUNTER_H