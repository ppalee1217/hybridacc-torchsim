#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/chrono.h>
#include <systemc.h>
#include "counter.h"
#include <memory>

namespace py = pybind11;

// Counter 測試台包裝類
class CounterTestBench {
private:
    std::unique_ptr<Counter> counter_inst;
    std::unique_ptr<sc_clock> clk;
    std::unique_ptr<sc_signal<bool>> reset;
    std::unique_ptr<sc_signal<bool>> set;
    std::unique_ptr<sc_signal<bool>> clear;
    std::unique_ptr<sc_signal<bool>> increase;
    std::unique_ptr<sc_signal<int>> set_value;
    std::unique_ptr<sc_signal<int>> count_out;
    std::unique_ptr<sc_signal<bool>> overflow;

public:
    CounterTestBench(const std::string& name = "counter_tb") {
        // 創建時鐘信號 (10ns 週期)
        clk = std::make_unique<sc_clock>("clk", 10, SC_NS);

        // 創建信號
        reset = std::make_unique<sc_signal<bool>>("reset");
        set = std::make_unique<sc_signal<bool>>("set");
        clear = std::make_unique<sc_signal<bool>>("clear");
        increase = std::make_unique<sc_signal<bool>>("increase");
        set_value = std::make_unique<sc_signal<int>>("set_value");
        count_out = std::make_unique<sc_signal<int>>("count_out");
        overflow = std::make_unique<sc_signal<bool>>("overflow");

        // 創建 Counter 實例
        counter_inst = std::make_unique<Counter>("counter");

        // 連接信號
        counter_inst->clk(*clk);
        counter_inst->reset(*reset);
        counter_inst->set(*set);
        counter_inst->clear(*clear);
        counter_inst->increase(*increase);
        counter_inst->set_value(*set_value);
        counter_inst->count_out(*count_out);
        counter_inst->overflow(*overflow);

        // 初始化信號
        reset->write(false);
        set->write(false);
        clear->write(false);
        increase->write(false);
        set_value->write(0);
    }

    // 控制方法
    void set_reset(bool value) {
        reset->write(value);
    }

    void set_set_signal(bool value) {
        set->write(value);
    }

    void set_clear_signal(bool value) {
        clear->write(value);
    }

    void set_increase_signal(bool value) {
        increase->write(value);
    }

    void set_set_value(int value) {
        set_value->write(value);
    }

    // 讀取輸出
    int get_count_out() const {
        return count_out->read();
    }

    bool get_overflow() const {
        return overflow->read();
    }

    // 模擬控制
    void run_simulation(double time_ns) {
        sc_start(time_ns, SC_NS);
    }

    void step_clock(int cycles = 1) {
        sc_start(cycles * 10, SC_NS);  // 10ns per cycle
    }

    void stop_simulation() {
        sc_stop();
    }

    double get_current_time() const {
        return sc_time_stamp().to_double();
    }

    // 便利方法
    void reset_counter() {
        set_reset(true);
        step_clock(1);
        set_reset(false);
        step_clock(1);
    }

    void clear_counter() {
        set_clear_signal(true);
        step_clock(1);
        set_clear_signal(false);
        step_clock(1);
    }

    void increase_counter() {
        set_increase_signal(true);
        step_clock(1);
        set_increase_signal(false);
        step_clock(1);
    }

    void set_counter_value(int value) {
        set_set_value(value);
        set_set_signal(true);
        step_clock(1);
        set_set_signal(false);
        step_clock(1);
    }
};

// SystemC 模擬控制類
class SystemCSimulation {
public:
    static void initialize() {
        // SystemC 已經在模塊構造時初始化，這裡可以做額外設置
    }

    static void start(double time_ns) {
        sc_start(time_ns, SC_NS);
    }

    static void stop() {
        sc_stop();
    }

    static double get_time() {
        return sc_time_stamp().to_double();
    }

    static bool is_running() {
        return !sc_end_of_simulation_invoked();
    }
};

// pybind11 綁定
PYBIND11_MODULE(counter_systemc, m) {
    m.doc() = "SystemC Counter Module Python Bindings";

    // SystemC 模擬控制
    py::class_<SystemCSimulation>(m, "SystemCSimulation")
        .def_static("initialize", &SystemCSimulation::initialize,
                   "Initialize SystemC simulation")
        .def_static("start", &SystemCSimulation::start,
                   "Start simulation for specified time (ns)")
        .def_static("stop", &SystemCSimulation::stop,
                   "Stop simulation")
        .def_static("get_time", &SystemCSimulation::get_time,
                   "Get current simulation time")
        .def_static("is_running", &SystemCSimulation::is_running,
                   "Check if simulation is running");

    // Counter 測試台
    py::class_<CounterTestBench>(m, "CounterTestBench")
        .def(py::init<const std::string&>(), py::arg("name") = "counter_tb",
             "Create Counter TestBench")

        // 控制方法
        .def("set_reset", &CounterTestBench::set_reset,
             "Set reset signal")
        .def("set_set_signal", &CounterTestBench::set_set_signal,
             "Set the set signal")
        .def("set_clear_signal", &CounterTestBench::set_clear_signal,
             "Set the clear signal")
        .def("set_increase_signal", &CounterTestBench::set_increase_signal,
             "Set the increase signal")
        .def("set_set_value", &CounterTestBench::set_set_value,
             "Set the value for set operation")

        // 讀取方法
        .def("get_count_out", &CounterTestBench::get_count_out,
             "Get counter output value")
        .def("get_overflow", &CounterTestBench::get_overflow,
             "Get overflow flag")

        // 模擬控制
        .def("run_simulation", &CounterTestBench::run_simulation,
             "Run simulation for specified time (ns)")
        .def("step_clock", &CounterTestBench::step_clock,
             py::arg("cycles") = 1, "Step clock for specified cycles")
        .def("stop_simulation", &CounterTestBench::stop_simulation,
             "Stop simulation")
        .def("get_current_time", &CounterTestBench::get_current_time,
             "Get current simulation time")

        // 便利方法
        .def("reset_counter", &CounterTestBench::reset_counter,
             "Reset the counter")
        .def("clear_counter", &CounterTestBench::clear_counter,
             "Clear the counter")
        .def("increase_counter", &CounterTestBench::increase_counter,
             "Increase counter by 1")
        .def("set_counter_value", &CounterTestBench::set_counter_value,
             "Set counter to specific value");
}