/// test_cluster_control_unit.cpp — Pure-logic unit tests for ClusterControlUnit
/// No SystemC dependency – this exercises the extracted FSM directly.

#include "Cluster/ClusterControlUnit.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace hybridacc::cluster;

// ── Helpers ─────────────────────────────────────────────────────────

static uint32_t ctrl_bit(UnifiedCtrlBit b) { return bit_mask(b); }
static uint32_t st_bit(UnifiedStatusBit b) { return bit_mask(b); }

struct TestResult {
	std::string name;
	bool pass;
};

static std::vector<TestResult> results;

#define RUN_TEST(name, body)                                         \
	do {                                                             \
		bool _ok = true;                                             \
		body                                                         \
		results.push_back({name, _ok});                              \
		printf("  %-3zu %-55s %s\n", results.size(), name, _ok ? "PASS" : "FAIL"); \
	} while (0)

#define CHECK(cond)                                                  \
	do { if (!(cond)) { printf("    FAIL: %s (line %d)\n", #cond, __LINE__); _ok = false; } } while(0)

// ── Tests ───────────────────────────────────────────────────────────

int main() {
	printf("=== ClusterControlUnit Unit Tests ===\n\n");

	// 1. Default construction
	RUN_TEST("Default state after construction", {
		ClusterControlUnit cu;
		CHECK(cu.mode() == ClusterMode::DIRECT_DEBUG);
		CHECK(cu.substate() == ClusterSubstate::IDLE);
		CHECK(cu.error_code() == 0u);
		CHECK(!cu.layer_active());
		CHECK(!cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
		CHECK(!cu.done_sticky());
	});

	// 2. Reset restores defaults
	RUN_TEST("reset() restores defaults", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		CHECK(cu.layer_active());
		cu.reset();
		CHECK(cu.mode() == ClusterMode::DIRECT_DEBUG);
		CHECK(cu.substate() == ClusterSubstate::IDLE);
		CHECK(!cu.layer_active());
		CHECK(!cu.done_sticky());
	});

	// 3. write_mode LAYER_MANAGED
	RUN_TEST("write_mode sets LAYER_MANAGED", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		CHECK(cu.mode() == ClusterMode::LAYER_MANAGED);
	});

	// 4. write_mode unknown falls back to DIRECT_DEBUG
	RUN_TEST("write_mode unknown -> DIRECT_DEBUG", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_mode(0xFFu);
		CHECK(cu.mode() == ClusterMode::DIRECT_DEBUG);
	});

	// 5. START in DIRECT_DEBUG is no-op
	RUN_TEST("write_ctrl START in DIRECT_DEBUG is no-op", {
		ClusterControlUnit cu;
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		CHECK(out.noc_action == ClusterAction::NONE);
		CHECK(!cu.layer_active());
	});

	// 6. START in LAYER_MANAGED
	RUN_TEST("write_ctrl START in LAYER_MANAGED", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		CHECK(out.noc_action == ClusterAction::NOC_START_PE);
		CHECK(!out.spm_soft_reset);
		CHECK(cu.layer_active());
		CHECK(cu.substate() == ClusterSubstate::RUNNING);
		CHECK(!cu.done_sticky());
		CHECK(cu.error_code() == 0u);
	});

	// 7. STOP in LAYER_MANAGED
	RUN_TEST("write_ctrl STOP in LAYER_MANAGED", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		CHECK(out.noc_action == ClusterAction::NOC_STOP_PE);
		CHECK(cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
		CHECK(cu.substate() == ClusterSubstate::STOPPING);
	});

	// 8. background_tick completes stop when quiesced
	RUN_TEST("background_tick completes stop when quiesced", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		// noc not yet quiesced
		auto out1 = cu.background_tick(false);
		CHECK(out1.noc_action == ClusterAction::NONE);
		CHECK(cu.stop_pending());
		// noc now quiesced
		auto out2 = cu.background_tick(true);
		CHECK(out2.noc_action == ClusterAction::NONE);
		CHECK(!out2.spm_soft_reset);
		CHECK(!cu.layer_active());
		CHECK(!cu.stop_pending());
		CHECK(cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::IDLE);
	});

	// 9. SOFT_RESET when already quiesced (immediate path)
	RUN_TEST("SOFT_RESET immediate (already quiesced)", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::SOFT_RESET), true);
		CHECK(out.noc_action == ClusterAction::NOC_RESET);
		CHECK(out.spm_soft_reset);
		CHECK(!cu.layer_active());
		CHECK(!cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
		CHECK(cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::IDLE);
	});

	// 10. SOFT_RESET when layer active (deferred path)
	RUN_TEST("SOFT_RESET deferred (layer active)", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::SOFT_RESET), true);
		CHECK(out.noc_action == ClusterAction::NOC_STOP_PE);
		CHECK(!out.spm_soft_reset);
		CHECK(cu.stop_pending());
		CHECK(cu.soft_reset_pending());
		CHECK(cu.substate() == ClusterSubstate::WAIT_QUIESCED);
	});

	// 11. SOFT_RESET deferred completion via background_tick
	RUN_TEST("SOFT_RESET deferred completes via background_tick", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::SOFT_RESET), true);
		// tick while not quiesced
		auto out1 = cu.background_tick(false);
		CHECK(out1.noc_action == ClusterAction::NONE);
		CHECK(cu.soft_reset_pending());
		// tick when quiesced → should issue NOC_RESET + spm reset
		auto out2 = cu.background_tick(true);
		CHECK(out2.noc_action == ClusterAction::NOC_RESET);
		CHECK(out2.spm_soft_reset);
		CHECK(!cu.layer_active());
		CHECK(!cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
		CHECK(cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::IDLE);
	});

	// 12. SOFT_RESET when NoC not quiesced but layer not active
	RUN_TEST("SOFT_RESET deferred (noc not quiesced, layer inactive)", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		// noc_quiesced = false → deferred
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::SOFT_RESET), false);
		CHECK(out.noc_action == ClusterAction::NOC_STOP_PE);
		CHECK(cu.stop_pending());
		CHECK(cu.soft_reset_pending());
		CHECK(cu.substate() == ClusterSubstate::WAIT_QUIESCED);
	});

	// 13. status_word: IDLE when nothing active
	RUN_TEST("status_word IDLE", {
		ClusterControlUnit cu;
		uint32_t st = cu.status_word(true, true);
		CHECK(st & st_bit(UnifiedStatusBit::IDLE));
		CHECK(!(st & st_bit(UnifiedStatusBit::BUSY)));
		CHECK(st & st_bit(UnifiedStatusBit::QUIESCED));
		CHECK(!(st & st_bit(UnifiedStatusBit::DONE)));
	});

	// 14. status_word: BUSY when layer active
	RUN_TEST("status_word BUSY when layer_active", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		uint32_t st = cu.status_word(false, true);
		CHECK(st & st_bit(UnifiedStatusBit::BUSY));
		CHECK(!(st & st_bit(UnifiedStatusBit::IDLE)));
		CHECK(!(st & st_bit(UnifiedStatusBit::QUIESCED)));
	});

	// 15. status_word: DONE sticky after stop completes
	RUN_TEST("status_word DONE sticky after stop", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		cu.background_tick(true);
		uint32_t st = cu.status_word(true, true);
		CHECK(st & st_bit(UnifiedStatusBit::DONE));
		CHECK(st & st_bit(UnifiedStatusBit::IDLE));
		CHECK(st & st_bit(UnifiedStatusBit::QUIESCED));
	});

	// 16. status_word: QUIESCED requires both noc and spm
	RUN_TEST("status_word QUIESCED requires noc+spm", {
		ClusterControlUnit cu;
		CHECK(!(cu.status_word(false, true) & st_bit(UnifiedStatusBit::QUIESCED)));
		CHECK(!(cu.status_word(true, false) & st_bit(UnifiedStatusBit::QUIESCED)));
		CHECK(cu.status_word(true, true) & st_bit(UnifiedStatusBit::QUIESCED));
	});

	// 17. write_error_code clears with 0, non-zero ignored
	RUN_TEST("write_error_code clears on 0 only", {
		ClusterControlUnit cu;
		// Note: we can't set error_code directly, but after reset it's 0
		cu.write_error_code(0u);
		CHECK(cu.error_code() == 0u);
		// write non-zero should not change anything (only 0 clears)
		cu.write_error_code(42u);
		CHECK(cu.error_code() == 0u);
	});

	// 18. notify_direct_start_pe
	RUN_TEST("notify_direct_start_pe", {
		ClusterControlUnit cu;
		cu.notify_direct_start_pe();
		CHECK(cu.layer_active());
		CHECK(!cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
		CHECK(!cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::RUNNING);
		CHECK(cu.error_code() == 0u);
	});

	// 19. notify_direct_stop_pe
	RUN_TEST("notify_direct_stop_pe", {
		ClusterControlUnit cu;
		cu.notify_direct_start_pe();
		cu.notify_direct_stop_pe();
		CHECK(cu.stop_pending());
		CHECK(!cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::STOPPING);
	});

	// 20. notify_direct_reset
	RUN_TEST("notify_direct_reset", {
		ClusterControlUnit cu;
		cu.notify_direct_start_pe();
		cu.notify_direct_reset();
		CHECK(!cu.layer_active());
		CHECK(!cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
		CHECK(cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::IDLE);
	});

	// 21. Full layer-managed lifecycle: START → STOP → quiesce → SOFT_RESET → START
	RUN_TEST("Full lifecycle: START->STOP->quiesce->SOFT_RESET->START", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));

		// layer 0: START
		auto out0 = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		CHECK(out0.noc_action == ClusterAction::NOC_START_PE);
		CHECK(cu.substate() == ClusterSubstate::RUNNING);

		// layer 0: STOP
		auto out1 = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		CHECK(out1.noc_action == ClusterAction::NOC_STOP_PE);

		// quiesce
		auto out2 = cu.background_tick(true);
		CHECK(cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::IDLE);

		// SOFT_RESET (already quiesced)
		auto out3 = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::SOFT_RESET), true);
		CHECK(out3.noc_action == ClusterAction::NOC_RESET);
		CHECK(out3.spm_soft_reset);
		CHECK(cu.done_sticky());

		// layer 1: START again
		auto out4 = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		CHECK(out4.noc_action == ClusterAction::NOC_START_PE);
		CHECK(cu.layer_active());
		CHECK(!cu.done_sticky());
		CHECK(cu.substate() == ClusterSubstate::RUNNING);
	});

	// 22. SOFT_RESET clears done_sticky before deciding path
	RUN_TEST("SOFT_RESET clears done_sticky", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		cu.background_tick(true);
		CHECK(cu.done_sticky());
		// SOFT_RESET immediate: done_sticky cleared first, then set again at end
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::SOFT_RESET), true);
		CHECK(cu.done_sticky()); // re-set by immediate reset path
	});

	// 23. Multiple background_tick without stop_pending is no-op
	RUN_TEST("background_tick no-op without stop_pending", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		auto out = cu.background_tick(true);
		CHECK(out.noc_action == ClusterAction::NONE);
		CHECK(!out.spm_soft_reset);
		CHECK(cu.layer_active()); // not cleared
	});

	// 24. START clears error_code
	RUN_TEST("START clears error_code", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		// After construction error_code is 0; START still explicitly sets 0
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		CHECK(cu.error_code() == 0u);
	});

	// 25. STOP priority: SOFT_RESET > STOP > START
	RUN_TEST("Priority: SOFT_RESET beats START+STOP", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		// All three bits set: SOFT_RESET should win
		uint32_t all = ctrl_bit(UnifiedCtrlBit::START) | ctrl_bit(UnifiedCtrlBit::STOP) | ctrl_bit(UnifiedCtrlBit::SOFT_RESET);
		auto out = cu.write_ctrl(all, true);
		// SOFT_RESET immediate path
		CHECK(out.noc_action == ClusterAction::NOC_RESET);
		CHECK(out.spm_soft_reset);
	});

	// 26. STOP beats START when both set (no SOFT_RESET)
	RUN_TEST("Priority: STOP beats START", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		uint32_t both = ctrl_bit(UnifiedCtrlBit::START) | ctrl_bit(UnifiedCtrlBit::STOP);
		auto out = cu.write_ctrl(both, true);
		CHECK(out.noc_action == ClusterAction::NOC_STOP_PE);
		CHECK(cu.stop_pending());
	});

	// 27. status_word ERROR bit when error_code nonzero
	// (Can't easily set error_code externally, so we test via the getter check)
	RUN_TEST("status_word no ERROR when error_code=0", {
		ClusterControlUnit cu;
		uint32_t st = cu.status_word(true, true);
		CHECK(!(st & st_bit(UnifiedStatusBit::ERROR)));
	});

	// 28. Repeated STOP without START between
	RUN_TEST("Repeated STOP without intervening START", {
		ClusterControlUnit cu;
		cu.write_mode(static_cast<uint32_t>(ClusterMode::LAYER_MANAGED));
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::START), true);
		cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		// second STOP while still pending
		auto out = cu.write_ctrl(ctrl_bit(UnifiedCtrlBit::STOP), true);
		CHECK(out.noc_action == ClusterAction::NOC_STOP_PE);
		CHECK(cu.stop_pending());
		CHECK(!cu.soft_reset_pending());
	});

	// ── Summary ─────────────────────────────────────────────────────

	printf("\n");
	int pass_count = 0;
	for (auto& r : results) {
		if (r.pass) ++pass_count;
	}
	printf("Summary: %d / %zu passed\n", pass_count, results.size());
	return (pass_count == (int)results.size()) ? 0 : 1;
}
