#pragma once

#include <systemc>
#include <array>

#include "Cluster/AddressGenerateUnit.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

template <int SPM_ADDR_BITS = 32, int NOC_TAG_BITS = 6, int DATA_BITS = 256>
class HybridDataDeliverUnit : public sc_module {
public:
	static constexpr int NUM_AGU = 4;
	static constexpr int NUM_SPM = 4;
	static constexpr int NUM_NOC = 4;
	static constexpr int NUM_SEND_PLANES = 3;
	static constexpr int RECV_PLANE = 3;
	static constexpr int NOC_ADDR_BITS = NOC_TAG_BITS + 1;
	static constexpr int DATA_BYTES = DATA_BITS / 8;

	enum PlaneId : int {
		PLANE_PS = 0,
		PLANE_PD = 1,
		PLANE_PLI = 2,
		PLANE_PLO = 3,
	};

	// --- Clock / Reset ---
	sc_in<bool> clk;
	sc_in<bool> reset_n;

	// --- SPM ports (0/1/2 read, 3 write) ---
	sc_vector<sc_out<sc_uint<SPM_ADDR_BITS>>> spm_addr;
	sc_vector<sc_out<bool>> spm_req;
	sc_vector<sc_out<bool>> spm_we;
	sc_vector<sc_out<sc_biguint<DATA_BITS>>> spm_wdata;
	sc_vector<sc_in<sc_biguint<DATA_BITS>>> spm_rdata;
	sc_vector<sc_in<bool>> spm_ready;

	// --- NoC ports (0/1/2 send, 3 recv) ---
	sc_vector<sc_out<sc_biguint<DATA_BITS>>> noc_out_data;
	sc_vector<sc_out<sc_uint<NOC_ADDR_BITS>>> noc_out_addr;
	sc_vector<sc_out<bool>> noc_out_valid;
	sc_vector<sc_in<bool>> noc_out_ready;

	sc_in<sc_biguint<DATA_BITS>> noc_in3_data;
	sc_in<sc_uint<NOC_ADDR_BITS>> noc_in3_addr;
	sc_in<bool> noc_in3_valid;
	sc_out<bool> noc_in3_ready;

	// --- MMIO ---
	sc_in<sc_uint<32>> mmio_addr;
	sc_in<bool> mmio_write;
	sc_in<sc_uint<32>> mmio_wdata;
	sc_out<sc_uint<32>> mmio_rdata;

	// --- Interrupt ---
	sc_out<bool> interrupt;

	// debug：目前仲裁到哪一個 AGU。
	sc_signal<sc_uint<3>> arb_state;

	// 4 個 AGU 實例（bank0~bank3）。
	sc_vector<AddressGenerateUnit> agus;

	SC_HAS_PROCESS(HybridDataDeliverUnit);
	explicit HybridDataDeliverUnit(sc_module_name name)
		: sc_module(name),
		  clk("clk"),
		  reset_n("reset_n"),
		  spm_addr("spm_addr", NUM_SPM),
		  spm_req("spm_req", NUM_SPM),
		  spm_we("spm_we", NUM_SPM),
		  spm_wdata("spm_wdata", NUM_SPM),
		  spm_rdata("spm_rdata", NUM_SPM),
		  spm_ready("spm_ready", NUM_SPM),
		  noc_out_data("noc_out_data", NUM_NOC),
		  noc_out_addr("noc_out_addr", NUM_NOC),
		  noc_out_valid("noc_out_valid", NUM_NOC),
		  noc_out_ready("noc_out_ready", NUM_NOC),
		  noc_in3_data("noc_in3_data"),
		  noc_in3_addr("noc_in3_addr"),
		  noc_in3_valid("noc_in3_valid"),
		  noc_in3_ready("noc_in3_ready"),
		  mmio_addr("mmio_addr"),
		  mmio_write("mmio_write"),
		  mmio_wdata("mmio_wdata"),
		  mmio_rdata("mmio_rdata"),
		  interrupt("interrupt"),
		  arb_state("arb_state"),
		  agus("agus", NUM_AGU),
		  agu_cfg_write_sig("agu_cfg_write_sig", NUM_AGU),
		  agu_cfg_addr_sig("agu_cfg_addr_sig", NUM_AGU),
		  agu_cfg_wdata_sig("agu_cfg_wdata_sig", NUM_AGU),
		  agu_cfg_rdata_sig("agu_cfg_rdata_sig", NUM_AGU),
		  agu_start_sig("agu_start_sig", NUM_AGU),
		  agu_stop_sig("agu_stop_sig", NUM_AGU),
		  agu_gen_valid_sig("agu_gen_valid_sig", NUM_AGU),
		  agu_gen_ready_sig("agu_gen_ready_sig", NUM_AGU),
		  agu_gen_addr_sig("agu_gen_addr_sig", NUM_AGU),
		  agu_gen_tag_sig("agu_gen_tag_sig", NUM_AGU),
		  agu_gen_ultra_sig("agu_gen_ultra_sig", NUM_AGU),
		  agu_gen_mask_sig("agu_gen_mask_sig", NUM_AGU),
		  agu_busy_sig("agu_busy_sig", NUM_AGU),
		  agu_done_sig("agu_done_sig", NUM_AGU),
		  agu_fsm_state_sig("agu_fsm_state_sig", NUM_AGU) {
		for (int i = 0; i < NUM_AGU; ++i) {
			agus[i].clk(clk);
			agus[i].reset_n(reset_n);
			agus[i].cfg_write(agu_cfg_write_sig[i]);
			agus[i].cfg_addr(agu_cfg_addr_sig[i]);
			agus[i].cfg_wdata(agu_cfg_wdata_sig[i]);
			agus[i].cfg_rdata(agu_cfg_rdata_sig[i]);
			agus[i].start(agu_start_sig[i]);
			agus[i].stop(agu_stop_sig[i]);
			agus[i].gen_valid(agu_gen_valid_sig[i]);
			agus[i].gen_ready(agu_gen_ready_sig[i]);
			agus[i].gen_addr(agu_gen_addr_sig[i]);
			agus[i].gen_tag(agu_gen_tag_sig[i]);
			agus[i].gen_ultra(agu_gen_ultra_sig[i]);
			agus[i].gen_mask(agu_gen_mask_sig[i]);
			agus[i].busy(agu_busy_sig[i]);
			agus[i].done(agu_done_sig[i]);
			agus[i].fsm_state(agu_fsm_state_sig[i]);
		}

		SC_METHOD(comb_mmio_read);
		sensitive << mmio_addr
				  << global_ctrl_reg
				  << global_status_reg
				  << plane_en_reg
				  << plane_mode_reg
				  << max_outstanding_reg
				  << arb_policy_reg
				  << err_code_reg
				  << err_info0_reg
				  << err_info1_reg
				  << counter_tx_pkt_reg
				  << counter_tx_byte_reg
				  << counter_rx_byte_reg
				  << counter_stall_reg;
		for (int i = 0; i < NUM_AGU; ++i) {
			sensitive << agu_cfg_rdata_sig[i];
		}

		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

private:
	sc_vector<sc_signal<bool>> agu_cfg_write_sig;
	sc_vector<sc_signal<sc_uint<8>>> agu_cfg_addr_sig;
	sc_vector<sc_signal<sc_uint<32>>> agu_cfg_wdata_sig;
	sc_vector<sc_signal<sc_uint<32>>> agu_cfg_rdata_sig;
	sc_vector<sc_signal<bool>> agu_start_sig;
	sc_vector<sc_signal<bool>> agu_stop_sig;

	sc_vector<sc_signal<bool>> agu_gen_valid_sig;
	sc_vector<sc_signal<bool>> agu_gen_ready_sig;
	sc_vector<sc_signal<sc_uint<32>>> agu_gen_addr_sig;
	sc_vector<sc_signal<sc_uint<16>>> agu_gen_tag_sig;
	sc_vector<sc_signal<bool>> agu_gen_ultra_sig;
	sc_vector<sc_signal<sc_uint<16>>> agu_gen_mask_sig;
	sc_vector<sc_signal<bool>> agu_busy_sig;
	sc_vector<sc_signal<bool>> agu_done_sig;
	sc_vector<sc_signal<sc_uint<2>>> agu_fsm_state_sig;

	sc_signal<sc_uint<32>> global_ctrl_reg;
	sc_signal<sc_uint<32>> global_status_reg;
	sc_signal<sc_uint<32>> plane_en_reg;
	sc_signal<sc_uint<32>> plane_mode_reg;
	sc_signal<sc_uint<32>> max_outstanding_reg;
	sc_signal<sc_uint<32>> arb_policy_reg;
	sc_signal<sc_uint<32>> err_code_reg;
	sc_signal<sc_uint<32>> err_info0_reg;
	sc_signal<sc_uint<32>> err_info1_reg;
	sc_signal<sc_uint<32>> counter_tx_pkt_reg;
	sc_signal<sc_uint<32>> counter_tx_byte_reg;
	sc_signal<sc_uint<32>> counter_rx_byte_reg;
	sc_signal<sc_uint<32>> counter_stall_reg;

	std::array<bool, NUM_SEND_PLANES> out_pending_reg{};
	std::array<sc_biguint<DATA_BITS>, NUM_SEND_PLANES> out_data_reg{};
	std::array<sc_uint<NOC_ADDR_BITS>, NUM_SEND_PLANES> out_addr_reg{};
	int rr_ptr = 0;

	void reset_internal() {
		global_ctrl_reg.write(0);
		global_status_reg.write(0);
		plane_en_reg.write(0xF);
		plane_mode_reg.write(0);
		max_outstanding_reg.write(16);
		arb_policy_reg.write(0);
		err_code_reg.write(0);
		err_info0_reg.write(0);
		err_info1_reg.write(0);
		counter_tx_pkt_reg.write(0);
		counter_tx_byte_reg.write(0);
		counter_rx_byte_reg.write(0);
		counter_stall_reg.write(0);
		interrupt.write(false);

		for (int i = 0; i < NUM_SPM; ++i) {
			spm_addr[i].write(0);
			spm_req[i].write(false);
			spm_we[i].write(false);
			spm_wdata[i].write(0);
		}
		for (int i = 0; i < NUM_NOC; ++i) {
			noc_out_data[i].write(0);
			noc_out_addr[i].write(0);
			noc_out_valid[i].write(false);
		}
		noc_in3_ready.write(false);
		arb_state.write(0);

		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			out_pending_reg[i] = false;
			out_data_reg[i] = 0;
			out_addr_reg[i] = 0;
		}
		rr_ptr = 0;

		for (int i = 0; i < NUM_AGU; ++i) {
			agu_cfg_write_sig[i].write(false);
			agu_cfg_addr_sig[i].write(0);
			agu_cfg_wdata_sig[i].write(0);
			agu_start_sig[i].write(false);
			agu_stop_sig[i].write(false);
			agu_gen_ready_sig[i].write(false);
		}
	}

	void route_mmio_to_agu(sc_uint<32> addr, sc_uint<32> wdata, bool is_write) {
		const uint32_t a = addr.to_uint();
		const int bank = static_cast<int>((a >> 8) & 0x3);
		const sc_uint<8> sub = static_cast<sc_uint<8>>(a & 0xFF);

		for (int i = 0; i < NUM_AGU; ++i) {
			agu_cfg_addr_sig[i].write(sub);
			agu_cfg_wdata_sig[i].write(wdata);
			agu_cfg_write_sig[i].write(false);
			agu_start_sig[i].write(false);
			agu_stop_sig[i].write(false);
		}

		if (bank < 0 || bank >= NUM_AGU || !is_write) {
			return;
		}

		if (sub == 0x20) {
			if (wdata[0]) agu_start_sig[bank].write(true);
			if (wdata[1]) agu_stop_sig[bank].write(true);
		}

		agu_cfg_write_sig[bank].write(true);
	}

	void handle_global_mmio_write(sc_uint<32> addr, sc_uint<32> wdata) {
		const uint32_t a = addr.to_uint();
		if (a < 0x800 || a > 0x8FF) {
			return;
		}

		switch (a) {
			case 0x800: {
				global_ctrl_reg.write(wdata);
				if (wdata[1]) {
					err_code_reg.write(0);
					err_info0_reg.write(0);
					err_info1_reg.write(0);
					for (int i = 0; i < NUM_SEND_PLANES; ++i) {
						out_pending_reg[i] = false;
					}
				}
				if (wdata[2]) {
					for (int i = 0; i < NUM_AGU; ++i) {
						agu_start_sig[i].write(true);
					}
				}
				if (wdata[3]) {
					for (int i = 0; i < NUM_AGU; ++i) {
						agu_stop_sig[i].write(true);
					}
				}
				break;
			}
			case 0x808: plane_en_reg.write(wdata); break;
			case 0x80C: plane_mode_reg.write(wdata); break;
			case 0x818: max_outstanding_reg.write(wdata); break;
			case 0x81C: arb_policy_reg.write(wdata); break;
			default: break;
		}
	}

	void comb_mmio_read() {
		const uint32_t a = mmio_addr.read().to_uint();
		sc_uint<32> r = 0;

		if (a < 0x400) {
			int bank = static_cast<int>((a >> 8) & 0x3);
			if (bank >= 0 && bank < NUM_AGU) {
				r = agu_cfg_rdata_sig[bank].read();
			}
		} else if (a >= 0x800 && a <= 0x8FF) {
			switch (a) {
				case 0x800: r = global_ctrl_reg.read(); break;
				case 0x804: r = global_status_reg.read(); break;
				case 0x808: r = plane_en_reg.read(); break;
				case 0x80C: r = plane_mode_reg.read(); break;
				case 0x810: r = NUM_NOC; break;
				case 0x814: r = DATA_BITS / NUM_NOC; break;
				case 0x818: r = max_outstanding_reg.read(); break;
				case 0x81C: r = arb_policy_reg.read(); break;
				case 0x820: r = err_code_reg.read(); break;
				case 0x824: r = err_info0_reg.read(); break;
				case 0x828: r = err_info1_reg.read(); break;
				case 0x82C: r = counter_tx_pkt_reg.read(); break;
				case 0x830: r = counter_tx_byte_reg.read(); break;
				case 0x834: r = counter_rx_byte_reg.read(); break;
				case 0x838: r = counter_stall_reg.read(); break;
				default: r = 0; break;
			}
		}

		mmio_rdata.write(r);
	}

	static sc_uint<NOC_ADDR_BITS> encode_noc_addr(sc_uint<16> tag, bool ultra) {
		sc_uint<NOC_ADDR_BITS> value = 0;
		const sc_uint<NOC_TAG_BITS> tag_n = static_cast<sc_uint<NOC_TAG_BITS>>(tag);
		value.range(NOC_TAG_BITS - 1, 0) = tag_n;
		value[NOC_TAG_BITS] = ultra ? 1 : 0;
		return value;
	}

	void seq_process() {
		reset_internal();
		wait();

		while (true) {
			noc_in3_ready.write(false);
			for (int i = 0; i < NUM_SPM; ++i) {
				spm_req[i].write(false);
				spm_we[i].write(false);
				spm_wdata[i].write(0);
			}
			for (int i = 0; i < NUM_NOC; ++i) {
				noc_out_valid[i].write(false);
				noc_out_data[i].write(0);
				noc_out_addr[i].write(0);
			}

			for (int i = 0; i < NUM_SEND_PLANES; ++i) {
				noc_out_valid[i].write(out_pending_reg[i]);
				noc_out_data[i].write(out_data_reg[i]);
				noc_out_addr[i].write(out_addr_reg[i]);
			}

			for (int i = 0; i < NUM_AGU; ++i) {
				agu_gen_ready_sig[i].write(false);
				agu_cfg_write_sig[i].write(false);
				agu_start_sig[i].write(false);
				agu_stop_sig[i].write(false);
			}

			route_mmio_to_agu(mmio_addr.read(), mmio_wdata.read(), mmio_write.read());
			if (mmio_write.read()) {
				handle_global_mmio_write(mmio_addr.read(), mmio_wdata.read());
			}

			bool any_busy = false;
			for (int i = 0; i < NUM_AGU; ++i) {
				any_busy = any_busy || agu_busy_sig[i].read();
			}

			sc_uint<32> status = global_status_reg.read();
			status[0] = any_busy;
			status[1] = !any_busy;
			status[2] = (err_code_reg.read() != 0);
			status[3] = false;
			global_status_reg.write(status);

			int selected = -1;
			const uint32_t plane_en = plane_en_reg.read().to_uint();
			for (int turn = 0; turn < NUM_AGU; ++turn) {
				const int cand = (rr_ptr + turn) % NUM_AGU;
				if (((plane_en >> cand) & 0x1u) == 0) {
					continue;
				}
				if (agu_gen_valid_sig[cand].read()) {
					selected = cand;
					break;
				}
			}

			if (selected >= 0) {
				arb_state.write(static_cast<sc_uint<3>>(selected));
				if (selected < NUM_SEND_PLANES) {
					if (out_pending_reg[selected]) {
						sc_uint<32> stall = counter_stall_reg.read();
						counter_stall_reg.write(stall + 1);
						status[3] = true;
						global_status_reg.write(status);
					} else if (spm_ready[selected].read()) {
						const sc_uint<32> addr = agu_gen_addr_sig[selected].read();
						const sc_uint<16> tag = agu_gen_tag_sig[selected].read();
						const bool ultra = agu_gen_ultra_sig[selected].read();

						spm_addr[selected].write(static_cast<sc_uint<SPM_ADDR_BITS>>(addr));
						spm_req[selected].write(true);
						spm_we[selected].write(false);
						agu_gen_ready_sig[selected].write(true);

						out_data_reg[selected] = spm_rdata[selected].read();
						out_addr_reg[selected] = encode_noc_addr(tag, ultra);
						out_pending_reg[selected] = true;

						rr_ptr = (selected + 1) % NUM_AGU;
					} else {
						sc_uint<32> stall = counter_stall_reg.read();
						counter_stall_reg.write(stall + 1);
						status[3] = true;
						global_status_reg.write(status);
					}
				} else {
					const bool can_recv = spm_ready[RECV_PLANE].read() && noc_in3_valid.read();
					if (can_recv) {
						const sc_uint<32> addr = agu_gen_addr_sig[RECV_PLANE].read();
						spm_addr[RECV_PLANE].write(static_cast<sc_uint<SPM_ADDR_BITS>>(addr));
						spm_req[RECV_PLANE].write(true);
						spm_we[RECV_PLANE].write(true);
						spm_wdata[RECV_PLANE].write(noc_in3_data.read());
						noc_in3_ready.write(true);
						agu_gen_ready_sig[RECV_PLANE].write(true);

						sc_uint<32> rx_b = counter_rx_byte_reg.read();
						counter_rx_byte_reg.write(rx_b + DATA_BYTES);
						rr_ptr = (selected + 1) % NUM_AGU;
					} else {
						sc_uint<32> stall = counter_stall_reg.read();
						counter_stall_reg.write(stall + 1);
						status[3] = true;
						global_status_reg.write(status);
					}
				}
			} else {
				arb_state.write(7);
			}

			for (int i = 0; i < NUM_SEND_PLANES; ++i) {
				if (!out_pending_reg[i]) {
					continue;
				}
				noc_out_valid[i].write(true);
				noc_out_data[i].write(out_data_reg[i]);
				noc_out_addr[i].write(out_addr_reg[i]);
				if (noc_out_ready[i].read()) {
					out_pending_reg[i] = false;
					sc_uint<32> tx_pkt = counter_tx_pkt_reg.read();
					sc_uint<32> tx_b = counter_tx_byte_reg.read();
					counter_tx_pkt_reg.write(tx_pkt + 1);
					counter_tx_byte_reg.write(tx_b + DATA_BYTES);
				}
			}

			interrupt.write(status[2] || status[1]);
			wait();
		}
	}
};

} // namespace cluster
} // namespace hybridacc
