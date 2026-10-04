// LoopController follows the ISA: every LOOPIN (count 1..1024, payload
// count-1) pushes a frame that its own LOOPEND pops (doc/ISA.md, Control).
// A small instruction stepper drives the controller the way IF_ID does and
// counts how often each body instruction runs.
#include <systemc>
#include <iostream>
#include <string>
#include <vector>

#include "PE/LoopController.hpp"

using namespace sc_core;
using hybridacc::pe::LoopController;

struct Inst {
    bool loop_in;
    uint16_t count;   // LOOPIN iterations (1..1024), encoded as count-1
    bool loop_end;    // LOOPEND flag carried by this instruction
    std::string body; // name of a body instruction, empty for LOOPIN/HALT
};

static Inst loopin(uint16_t count) { return {true, count, false, ""}; }
static Inst body(const std::string& name, bool loop_end = false) { return {false, 0, loop_end, name}; }

SC_MODULE(Stepper) {
    sc_in<bool> clk;
    sc_out<bool> reset_n, stage_reset, loop_in_en, loop_end_en;
    sc_out<uint16_t> pc_in, count_in;
    sc_in<uint16_t> pc_out;
    sc_in<bool> jump;

    std::vector<Inst> program;
    std::vector<std::string> executed;
    bool finished = false;

    SC_CTOR(Stepper) { SC_THREAD(run); }

    void run() {
        stage_reset.write(false);
        loop_in_en.write(false);
        loop_end_en.write(false);
        reset_n.write(false);
        wait(clk.posedge_event());
        wait(clk.posedge_event());
        wait(1, SC_NS);
        reset_n.write(true);
        wait(clk.posedge_event());
        wait(1, SC_NS);

        size_t pc = 0;
        for (size_t steps = 0; pc < program.size() && steps < 100000; ++steps) {
            const Inst& inst = program[pc];
            loop_in_en.write(inst.loop_in);
            count_in.write(inst.loop_in ? static_cast<uint16_t>(inst.count - 1u) : 0u);
            pc_in.write(static_cast<uint16_t>(pc + 1u));
            loop_end_en.write(inst.loop_end);
            wait(2, SC_NS); // combinational jump / pc_out settle
            const bool take = inst.loop_end && jump.read();
            const size_t target = pc_out.read();
            if (!inst.body.empty()) executed.push_back(inst.body);
            wait(clk.posedge_event()); // controller samples this instruction
            wait(1, SC_NS);
            pc = take ? target : pc + 1u;
        }
        loop_in_en.write(false);
        loop_end_en.write(false);
        finished = pc == program.size();
        sc_stop();
    }
};

static size_t count_of(const std::vector<std::string>& v, const std::string& name) {
    size_t n = 0;
    for (const auto& s : v) n += (s == name);
    return n;
}

struct Case {
    std::string name;
    std::vector<Inst> program;
    std::vector<std::pair<std::string, size_t>> expected;
};

static bool run_case(const Case& c) {
    sc_clock clk(("clk_" + c.name).c_str(), 10, SC_NS);
    sc_signal<bool> reset_n, stage_reset, loop_in_en, loop_end_en, jump;
    sc_signal<uint16_t> pc_in, count_in, pc_out;

    LoopController lc(("lc_" + c.name).c_str());
    lc.clk(clk); lc.reset_n(reset_n); lc.stage_reset(stage_reset);
    lc.pc_in(pc_in); lc.count_in(count_in);
    lc.loop_in_en(loop_in_en); lc.loop_end_en(loop_end_en);
    lc.pc_out(pc_out); lc.jump(jump);

    Stepper st(("st_" + c.name).c_str());
    st.clk(clk); st.reset_n(reset_n); st.stage_reset(stage_reset);
    st.loop_in_en(loop_in_en); st.loop_end_en(loop_end_en);
    st.pc_in(pc_in); st.count_in(count_in); st.pc_out(pc_out); st.jump(jump);
    st.program = c.program;

    sc_start();

    bool ok = st.finished && lc.loopstack.empty();
    std::cout << c.name << ":";
    for (const auto& [name, want] : c.expected) {
        const size_t got = count_of(st.executed, name);
        std::cout << " " << name << "=" << got << "/" << want;
        ok = ok && got == want;
    }
    std::cout << " finished=" << st.finished << " stack_empty=" << lc.loopstack.empty()
              << (ok ? "  PASS" : "  FAIL") << "\n";
    return ok;
}

int sc_main(int argc, char* argv[]) {
    // Each case runs in its own process (one elaboration per SystemC run).
    const std::vector<Case> cases = {
        {"nested_2_1", {loopin(2), loopin(1), body("A", true), body("B", true), body("H")},
         {{"A", 2}, {"B", 2}, {"H", 1}}},
        {"nested_3_2", {loopin(3), loopin(2), body("A", true), body("B", true), body("H")},
         {{"A", 6}, {"B", 3}, {"H", 1}}},
        {"nested_1_5", {loopin(1), loopin(5), body("A", true), body("B", true), body("H")},
         {{"A", 5}, {"B", 1}, {"H", 1}}},
        {"triple_2_1_1", {loopin(2), loopin(1), loopin(1), body("A", true), body("B", true),
                          body("C", true), body("H")},
         {{"A", 2}, {"B", 2}, {"C", 2}, {"H", 1}}},
        {"single_1024", {loopin(1024), body("A", true), body("H")},
         {{"A", 1024}, {"H", 1}}},
    };
    if (argc < 2) {
        std::cerr << "usage: test_loop_controller <case index 0.." << cases.size() - 1 << ">\n";
        return 2;
    }
    const size_t index = std::stoul(argv[1]);
    if (index >= cases.size()) return 2;
    return run_case(cases[index]) ? 0 : 1;
}
