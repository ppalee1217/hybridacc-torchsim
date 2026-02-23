#pragma once

#include <systemc>
#include <array>
#include <deque>

#include "Cluster/AddressGenerateUnit.hpp"
#include "utils.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

template <int SPM_ADDR_BITS = 32, int NOC_TAG_BITS = 6, int DATA_BITS = 192>
class HybridDataDeliverUnit : public sc_module {
public:
	static constexpr int NUM_AGU = 4;
	static constexpr int NUM_SPM = 4;
	static constexpr int NUM_NOC = 4;
	static constexpr int NUM_SEND_PLANES = 3;
	static constexpr int RECV_PLANE = 3;
	static constexpr int NOC_ADDR_BITS = NOC_TAG_BITS + 1;
	static constexpr int DATA_BYTES = DATA_BITS / 8;
	using noc_req_payload_t = request_t<sc_biguint<DATA_BITS>, uint16_t>;
	using noc_addr_payload_t = noc_addr_req_t;
	using noc_resp_payload_t = response_t<sc_biguint<DATA_BITS>>;

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

	// --- NoC ports (typed valid/ready interfaces) ---
	VRDOF<noc_req_payload_t> noc_ps_out;
	VRDOF<noc_req_payload_t> noc_pd_out;
	VRDOF<noc_req_payload_t> noc_pli_out;
	VRDOF<noc_addr_payload_t> noc_plo_out;
	VRDIF<noc_resp_payload_t> noc_plo_in;

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
		  noc_ps_out("noc_ps_out"),
		  noc_pd_out("noc_pd_out"),
		  noc_pli_out("noc_pli_out"),
		  noc_plo_out("noc_plo_out"),
		  noc_plo_in("noc_plo_in"),
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

	std::array<std::deque<sc_uint<NOC_ADDR_BITS>>, NUM_SEND_PLANES> tx_tag_fifo_reg{};
	std::array<std::deque<sc_biguint<DATA_BITS>>, NUM_SEND_PLANES> tx_data_fifo_reg{};
	std::deque<sc_uint<SPM_ADDR_BITS>> plo_addr_fifo_reg{};

	unsigned fifo_limit() const {
		const uint32_t cfg = max_outstanding_reg.read().to_uint();
		return (cfg == 0) ? 1U : cfg;
	}

	void clear_channel_fifos() {
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			tx_tag_fifo_reg[i].clear();
			tx_data_fifo_reg[i].clear();
		}
		plo_addr_fifo_reg.clear();
	}

	static noc_req_payload_t make_noc_req(sc_biguint<DATA_BITS> data, sc_uint<NOC_ADDR_BITS> addr) {
		noc_req_payload_t req{};
		req.data = data;
		req.addr = static_cast<uint16_t>(addr.to_uint());
		req.mask = static_cast<size_t>(~0ULL);
		return req;
	}

	static noc_addr_payload_t make_noc_plo_req(sc_uint<16> tag, bool ultra) {
		noc_addr_payload_t req{};
		sc_uint<NOC_ADDR_BITS> addr = 0;
		const sc_uint<NOC_TAG_BITS> tag_n = static_cast<sc_uint<NOC_TAG_BITS>>(tag);
		addr.range(NOC_TAG_BITS - 1, 0) = tag_n;
		addr[NOC_TAG_BITS] = ultra ? 1 : 0;
		req.addr = static_cast<uint16_t>(addr.to_uint());
		return req;
	}

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
		noc_ps_out.data_out.write(noc_req_payload_t{});
		noc_ps_out.valid_out.write(false);
		noc_pd_out.data_out.write(noc_req_payload_t{});
		noc_pd_out.valid_out.write(false);
		noc_pli_out.data_out.write(noc_req_payload_t{});
		noc_pli_out.valid_out.write(false);
		noc_plo_out.data_out.write(noc_addr_payload_t{});
		noc_plo_out.valid_out.write(false);
		noc_plo_in.ready_out.write(false);
		arb_state.write(0);

		clear_channel_fifos();

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
						clear_channel_fifos();
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
				case 0x810: r = 4; break;
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
			noc_ps_out.valid_out.write(false);
			noc_ps_out.data_out.write(noc_req_payload_t{});
			noc_pd_out.valid_out.write(false);
			noc_pd_out.data_out.write(noc_req_payload_t{});
			noc_pli_out.valid_out.write(false);
			noc_pli_out.data_out.write(noc_req_payload_t{});
			noc_plo_out.valid_out.write(false);
			noc_plo_out.data_out.write(noc_addr_payload_t{});
			noc_plo_in.ready_out.write(false);

			for (int i = 0; i < NUM_SPM; ++i) {
				spm_req[i].write(false);
				spm_we[i].write(false);
				spm_wdata[i].write(0);
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

			const uint32_t plane_en = plane_en_reg.read().to_uint();
			const bool plane_on_ps = ((plane_en >> PLANE_PS) & 0x1u) != 0;
			const bool plane_on_pd = ((plane_en >> PLANE_PD) & 0x1u) != 0;
			const bool plane_on_pli = ((plane_en >> PLANE_PLI) & 0x1u) != 0;
			const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;

			unsigned stall_inc = 0;
			unsigned tx_pkt_inc = 0;
			unsigned tx_byte_inc = 0;
			unsigned rx_byte_inc = 0;

			auto run_send_stage0 = [&](int lane, bool plane_enabled) {
				if (!plane_enabled || !agu_gen_valid_sig[lane].read()) {
					return;
				}
				const unsigned limit = fifo_limit();
				const bool room = tx_tag_fifo_reg[lane].size() < limit && tx_data_fifo_reg[lane].size() < limit;
				if (room && spm_ready[lane].read()) {
					const sc_uint<32> addr = agu_gen_addr_sig[lane].read();
					const sc_uint<16> tag = agu_gen_tag_sig[lane].read();
					const bool ultra = agu_gen_ultra_sig[lane].read();

					spm_addr[lane].write(static_cast<sc_uint<SPM_ADDR_BITS>>(addr));
					spm_req[lane].write(true);
					spm_we[lane].write(false);
					agu_gen_ready_sig[lane].write(true);

					tx_tag_fifo_reg[lane].push_back(encode_noc_addr(tag, ultra));
					tx_data_fifo_reg[lane].push_back(spm_rdata[lane].read());
				} else {
					stall_inc++;
				}
			};

			run_send_stage0(PLANE_PS, plane_on_ps);
			run_send_stage0(PLANE_PD, plane_on_pd);
			run_send_stage0(PLANE_PLI, plane_on_pli);

			auto run_send_stage1 = [&](int lane, VRDOF<noc_req_payload_t>& out_if) {
				if (tx_tag_fifo_reg[lane].empty() || tx_data_fifo_reg[lane].empty()) {
					return;
				}
				const noc_req_payload_t req = make_noc_req(tx_data_fifo_reg[lane].front(), tx_tag_fifo_reg[lane].front());
				out_if.data_out.write(req);
				out_if.valid_out.write(true);
				if (out_if.ready_in.read()) {
					tx_tag_fifo_reg[lane].pop_front();
					tx_data_fifo_reg[lane].pop_front();
					tx_pkt_inc++;
					tx_byte_inc += DATA_BYTES;
				}
			};

			run_send_stage1(PLANE_PS, noc_ps_out);
			run_send_stage1(PLANE_PD, noc_pd_out);
			run_send_stage1(PLANE_PLI, noc_pli_out);

			if (plane_on_plo && agu_gen_valid_sig[RECV_PLANE].read()) {
				const bool room = plo_addr_fifo_reg.size() < fifo_limit();
				noc_addr_payload_t req = make_noc_plo_req(agu_gen_tag_sig[RECV_PLANE].read(), agu_gen_ultra_sig[RECV_PLANE].read());
				noc_plo_out.data_out.write(req);
				noc_plo_out.valid_out.write(true);

				if (room && noc_plo_out.ready_in.read()) {
					agu_gen_ready_sig[RECV_PLANE].write(true);
					plo_addr_fifo_reg.push_back(static_cast<sc_uint<SPM_ADDR_BITS>>(agu_gen_addr_sig[RECV_PLANE].read()));
				} else if (!room || !noc_plo_out.ready_in.read()) {
					stall_inc++;
				}
			}

			const bool plo_rsp_valid = plane_on_plo && noc_plo_in.valid_in.read();
			const bool plo_addr_avail = !plo_addr_fifo_reg.empty();
			const bool can_write_spm = plo_addr_avail && spm_ready[RECV_PLANE].read();
			noc_plo_in.ready_out.write(plo_rsp_valid && can_write_spm);
			if (plo_rsp_valid) {
				if (can_write_spm) {
					spm_addr[RECV_PLANE].write(plo_addr_fifo_reg.front());
					spm_req[RECV_PLANE].write(true);
					spm_we[RECV_PLANE].write(true);
					spm_wdata[RECV_PLANE].write(noc_plo_in.data_in.read().data);
					plo_addr_fifo_reg.pop_front();
					rx_byte_inc += DATA_BYTES;
				} else {
					stall_inc++;
				}
			}

			int dbg = 7;
			for (int i = 0; i < NUM_AGU; ++i) {
				if ((((plane_en >> i) & 0x1u) != 0) && agu_gen_valid_sig[i].read()) {
					dbg = i;
					break;
				}
			}
			arb_state.write(static_cast<sc_uint<3>>(dbg));

			if (stall_inc > 0) {
				sc_uint<32> stall = counter_stall_reg.read();
				counter_stall_reg.write(stall + stall_inc);
				status[3] = true;
			}
			if (tx_pkt_inc > 0) {
				sc_uint<32> tx_pkt = counter_tx_pkt_reg.read();
				counter_tx_pkt_reg.write(tx_pkt + tx_pkt_inc);
			}
			if (tx_byte_inc > 0) {
				sc_uint<32> tx_b = counter_tx_byte_reg.read();
				counter_tx_byte_reg.write(tx_b + tx_byte_inc);
			}
			if (rx_byte_inc > 0) {
				sc_uint<32> rx_b = counter_rx_byte_reg.read();
				counter_rx_byte_reg.write(rx_b + rx_byte_inc);
			}

			global_status_reg.write(status);

			interrupt.write(status[2] || status[1]);
			wait();
		}
	}
};

} // namespace cluster
} // namespace hybridacc
