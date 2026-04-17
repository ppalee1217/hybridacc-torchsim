#pragma once

#include "ControlTypes.hpp"
#include <cstdint>

namespace hybridacc {
namespace cluster {

/// Abstract action that the cluster controller wants the parent
/// (ComputeCluster) to translate into a concrete NoC command.
enum class ClusterAction : uint32_t {
	NONE = 0,
	NOC_START_PE,
	NOC_STOP_PE,
	NOC_RESET,
};

/// Output produced by a single-cycle tick of the cluster controller.
struct ClusterCycleOutput {
	ClusterAction noc_action = ClusterAction::NONE;
	bool spm_soft_reset = false;
};

/// Pure-logic cluster lifecycle controller.
///
/// This class encapsulates the FSM and status logic that was previously
/// inlined inside ComputeCluster::seq_ahb_ctrl().  It is intentionally
/// *not* an SC_MODULE – ComputeCluster calls its methods synchronously
/// within its own SC_CTHREAD so that no extra delta-cycle latency is
/// introduced.
class ClusterControlUnit {
public:
	// ── Readable state ──────────────────────────────────────────────
	ClusterMode       mode()               const { return mode_; }
	ClusterSubstate   substate()           const { return substate_; }
	uint32_t          error_code()         const { return error_code_; }
	bool              layer_active()       const { return layer_active_; }
	bool              stop_pending()       const { return stop_pending_; }
	bool              soft_reset_pending() const { return soft_reset_pending_; }
	bool              done_sticky()        const { return done_sticky_; }

	// ── Reset ───────────────────────────────────────────────────────
	void reset() {
		mode_               = ClusterMode::DIRECT_DEBUG;
		substate_           = ClusterSubstate::IDLE;
		error_code_         = 0u;
		layer_active_       = false;
		stop_pending_       = false;
		soft_reset_pending_ = false;
		done_sticky_        = false;
	}

	// ── Background tick (called once per cycle, before AHB) ─────────
	ClusterCycleOutput background_tick(bool noc_quiesced) {
		ClusterCycleOutput out;
		if (stop_pending_ && noc_quiesced) {
			if (soft_reset_pending_) {
				out.noc_action      = ClusterAction::NOC_RESET;
				out.spm_soft_reset  = true;
				soft_reset_pending_ = false;
			}
			layer_active_  = false;
			stop_pending_  = false;
			done_sticky_   = true;
			substate_      = ClusterSubstate::IDLE;
			error_code_    = 0u;
		}
		return out;
	}

	// ── MMIO writes ─────────────────────────────────────────────────
	void write_mode(uint32_t wdata) {
		mode_ = (wdata == static_cast<uint32_t>(ClusterMode::LAYER_MANAGED))
			? ClusterMode::LAYER_MANAGED
			: ClusterMode::DIRECT_DEBUG;
	}

	ClusterCycleOutput write_ctrl(uint32_t wdata, bool noc_quiesced) {
		ClusterCycleOutput out;
		const bool is_lm         = (mode_ == ClusterMode::LAYER_MANAGED);
		const bool req_start     = (wdata & bit_mask(UnifiedCtrlBit::START))     != 0u;
		const bool req_stop      = (wdata & bit_mask(UnifiedCtrlBit::STOP))      != 0u;
		const bool req_soft_rst  = (wdata & bit_mask(UnifiedCtrlBit::SOFT_RESET))!= 0u;

		if (is_lm && req_soft_rst) {
			done_sticky_ = false;
			if (layer_active_ || !noc_quiesced) {
				out.noc_action      = ClusterAction::NOC_STOP_PE;
				stop_pending_       = true;
				soft_reset_pending_ = true;
				substate_           = ClusterSubstate::WAIT_QUIESCED;
			} else {
				out.noc_action      = ClusterAction::NOC_RESET;
				out.spm_soft_reset  = true;
				layer_active_       = false;
				stop_pending_       = false;
				soft_reset_pending_ = false;
				done_sticky_        = true;
				substate_           = ClusterSubstate::IDLE;
				error_code_         = 0u;
			}
		} else if (is_lm && req_stop) {
			out.noc_action      = ClusterAction::NOC_STOP_PE;
			stop_pending_       = true;
			soft_reset_pending_ = false;
			done_sticky_        = false;
			substate_           = ClusterSubstate::STOPPING;
		} else if (is_lm && req_start) {
			out.noc_action      = ClusterAction::NOC_START_PE;
			layer_active_       = true;
			stop_pending_       = false;
			soft_reset_pending_ = false;
			done_sticky_        = false;
			substate_           = ClusterSubstate::RUNNING;
			error_code_         = 0u;
		}
		return out;
	}

	void write_error_code(uint32_t wdata) {
		if (wdata == 0u) error_code_ = 0u;
	}

	// ── Direct-NOC-command mirror (DIRECT_DEBUG mode) ───────────────
	void notify_direct_start_pe() {
		layer_active_       = true;
		stop_pending_       = false;
		soft_reset_pending_ = false;
		done_sticky_        = false;
		substate_           = ClusterSubstate::RUNNING;
		error_code_         = 0u;
	}

	void notify_direct_stop_pe() {
		stop_pending_ = true;
		done_sticky_  = false;
		substate_     = ClusterSubstate::STOPPING;
	}

	void notify_direct_reset() {
		layer_active_       = false;
		stop_pending_       = false;
		soft_reset_pending_ = false;
		done_sticky_        = true;
		substate_           = ClusterSubstate::IDLE;
		error_code_         = 0u;
	}

	// ── Status word ─────────────────────────────────────────────────
	uint32_t status_word(bool noc_quiesced, bool spm_quiesced) const {
		const bool busy     = layer_active_ || stop_pending_ || soft_reset_pending_;
		const bool quiesced = !busy && noc_quiesced && spm_quiesced;

		uint32_t st = 0u;
		if (!busy)       st |= bit_mask(UnifiedStatusBit::IDLE);
		if (busy)        st |= bit_mask(UnifiedStatusBit::BUSY);
		if (done_sticky_)st |= bit_mask(UnifiedStatusBit::DONE);
		if (quiesced)    st |= bit_mask(UnifiedStatusBit::QUIESCED);
		if (error_code_) st |= bit_mask(UnifiedStatusBit::ERROR);
		return st;
	}

private:
	ClusterMode     mode_               = ClusterMode::DIRECT_DEBUG;
	ClusterSubstate substate_           = ClusterSubstate::IDLE;
	uint32_t        error_code_         = 0u;
	bool            layer_active_       = false;
	bool            stop_pending_       = false;
	bool            soft_reset_pending_ = false;
	bool            done_sticky_        = false;
};

} // namespace cluster
} // namespace hybridacc
