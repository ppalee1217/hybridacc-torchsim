#pragma once

#include <systemc>
#include <array>
#include <deque>
#include <string>
#include <utility>

#include "Cluster/AddressGenerateUnit.hpp"
#include "Utils/utils.hpp"
#include "Utils/FIFO.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc {
namespace cluster {

enum PlaneId : int {
	PLANE_PS = 0,
	PLANE_PD = 1,
	PLANE_PLI = 2,
	PLANE_PLO = 3,
};

enum class HdduStatusBit : int {
	IDLE = 0,
	BUSY = 1,
	DONE = 2,
	STALL = 3,
	ERROR = 4,
};

enum class HdduCtrllBit : int {
	CTRL_RESET = 0,
	CTRL_START = 1,
	CTRL_STOP = 2,
};

enum class HdduErrorCode : int {
	NONE = 0,
	AGU_ERROR = 1,
	NOC_ERROR = 2,
	SPM_ERROR = 3,
};

enum class ArbPolicy : int {
	FIXED = 0,
	ROUND_ROBIN = 1,
};


template <int SPM_ADDR_BITS = 32, int NOC_TAG_BITS = 6, int DATA_BITS = 192>
class HybridDataDeliverUnit : public sc_module {
public:
	static constexpr int NUM_AGU = 4; // 4 AGUs for 4 planes (PS, PD, PLI, PLO)
	static constexpr int NUM_SPM = 4; // 4 SPM ports (0/1/2 read, 3 write)
	static constexpr int NUM_NOC = 4; // 4 NoC channels (PS, PD, PLI, PLO)
	static constexpr int NUM_SEND_PLANES = 3; // PS/PD/PLI can send to NoC, PLO only receives from NoC
	static constexpr int RECV_PLANE = 3; // Only PLO receives from NoC
	static constexpr int NOC_ADDR_BITS = NOC_TAG_BITS + 1;
	static constexpr int DATA_BYTES = DATA_BITS / 8;
	static constexpr int FIFO_DEPTH = 16; // Depth of internal FIFOs for tracking in-flight transactions

	static constexpr uint32_t MMIO_AGU_BASE = 0x000;
	static constexpr uint32_t MMIO_AGU_BANK_STRIDE = 0x100;
	static constexpr uint32_t MMIO_AGU_SIZE = NUM_AGU * MMIO_AGU_BANK_STRIDE;
	static constexpr uint32_t MMIO_AGU_BANK_MASK = 0x3;
	static constexpr uint32_t MMIO_AGU_SUB_MASK = 0xFF;
	static constexpr uint32_t MMIO_AGU_CTRL_OFFSET = 0x20;

	static constexpr uint32_t MMIO_GLOBAL_BASE = 0x800;
	static constexpr uint32_t MMIO_GLOBAL_END = 0x8FF;
	static constexpr uint32_t MMIO_GLOBAL_CTRL = 0x800;
	static constexpr uint32_t MMIO_GLOBAL_STATUS = 0x804;
	static constexpr uint32_t MMIO_GLOBAL_PLANE_EN = 0x808;
	static constexpr uint32_t MMIO_GLOBAL_PLANE_MODE = 0x80C;
	static constexpr uint32_t MMIO_GLOBAL_NUM_PLANES = 0x810;
	static constexpr uint32_t MMIO_GLOBAL_PORT_WIDTH = 0x814;
	static constexpr uint32_t MMIO_GLOBAL_ARB_POLICY = 0x818;
	static constexpr uint32_t MMIO_GLOBAL_ERR_CODE = 0x81c;
	static constexpr uint32_t MMIO_GLOBAL_ERR_INFO0 = 0x820;
	static constexpr uint32_t MMIO_GLOBAL_ERR_INFO1 = 0x824;
	static constexpr uint32_t MMIO_GLOBAL_COUNTER_TX_PKT = 0x828;
	static constexpr uint32_t MMIO_GLOBAL_COUNTER_TX_BYTE = 0x82C;
	static constexpr uint32_t MMIO_GLOBAL_COUNTER_RX_BYTE = 0x830;
	static constexpr uint32_t MMIO_GLOBAL_COUNTER_STALL = 0x834;

	static constexpr uint32_t DEFAULT_PLANE_EN = (1u << NUM_AGU) - 1u;
	using noc_req_payload_t = request_t<sc_biguint<DATA_BITS>, uint16_t>;
	using noc_addr_payload_t = noc_addr_req_t;
	using noc_resp_payload_t = response_t<sc_biguint<DATA_BITS>>;
	using spm_req_payload_t = spm_request_t<SPM_ADDR_BITS, DATA_BITS>;
	using spm_resp_payload_t = spm_response_t<DATA_BITS>;

	// --- Clock / Reset ---
	sc_in<bool> clk;
	sc_in<bool> reset_n;

	// --- SPM request/response ports (0/1/2 read, 3 write) ---
	sc_vector<sc_out<bool>> spm_req_valid;
	sc_vector<sc_in<bool>> spm_req_ready;
	sc_vector<sc_out<spm_req_payload_t>> spm_req_payload;
	sc_vector<sc_in<bool>> spm_resp_valid;
	sc_vector<sc_out<bool>> spm_resp_ready;
	sc_vector<sc_in<spm_resp_payload_t>> spm_resp_payload;

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
		  spm_req_valid("spm_req_valid", NUM_SPM),
		  spm_req_ready("spm_req_ready", NUM_SPM),
		  spm_req_payload("spm_req_payload", NUM_SPM),
		  spm_resp_valid("spm_resp_valid", NUM_SPM),
		  spm_resp_ready("spm_resp_ready", NUM_SPM),
		  spm_resp_payload("spm_resp_payload", NUM_SPM),
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
		  agu_fsm_state_sig("agu_fsm_state_sig", NUM_AGU),
		  noc_req_fifo_empty_out("noc_req_fifo_empty_out", NUM_SEND_PLANES),
		  noc_req_fifo_full_out("noc_req_fifo_full_out", NUM_SEND_PLANES),
		  noc_req_fifo_data_out("noc_req_fifo_data_out", NUM_SEND_PLANES),
		  noc_req_fifo_data_in("noc_req_fifo_data_in", NUM_SEND_PLANES),
		  noc_req_fifo_push_in("noc_req_fifo_push_in", NUM_SEND_PLANES),
		  noc_req_fifo_pop_in("noc_req_fifo_pop_in", NUM_SEND_PLANES),
		  noc_req_fifo_clear_sig("noc_req_fifo_clear_sig", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_empty_out("read_noc_addr_wait_fifo_empty_out", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_full_out("read_noc_addr_wait_fifo_full_out", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_data_out("read_noc_addr_wait_fifo_data_out", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_data_in("read_noc_addr_wait_fifo_data_in", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_push_in("read_noc_addr_wait_fifo_push_in", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_pop_in("read_noc_addr_wait_fifo_pop_in", NUM_SEND_PLANES),
		  read_noc_addr_wait_fifo_clear_sig("read_noc_addr_wait_fifo_clear_sig", NUM_SEND_PLANES),
		  write_addr_fifo_empty_out("write_addr_fifo_empty_out"),
		  write_addr_fifo_full_out("write_addr_fifo_full_out"),
		  write_addr_fifo_data_out("write_addr_fifo_data_out"),
		  write_addr_fifo_data_in("write_addr_fifo_data_in"),
		  write_addr_fifo_push_in("write_addr_fifo_push_in"),
		  write_addr_fifo_pop_in("write_addr_fifo_pop_in"),
		  write_addr_fifo_clear_sig("write_addr_fifo_clear_sig"),
		  spm_req_fifo_empty_out("spm_req_fifo_empty_out"),
		  spm_req_fifo_full_out("spm_req_fifo_full_out"),
		  spm_req_fifo_data_out("spm_req_fifo_data_out"),
		  spm_req_fifo_data_in("spm_req_fifo_data_in"),
		  spm_req_fifo_push_in("spm_req_fifo_push_in"),
		  spm_req_fifo_pop_in("spm_req_fifo_pop_in"),
		  spm_req_fifo_clear_sig("spm_req_fifo_clear_sig"),
		  noc_req_fifo("noc_req_fifo"),
		  read_noc_addr_wait_fifo("read_noc_addr_wait_fifo"),
		  write_addr_fifo("write_addr_fifo", FIFO_DEPTH),
		  spm_req_fifo("spm_req_fifo", FIFO_DEPTH)
		  {

		// Initialize internal FIFOs
		noc_req_fifo.init(NUM_SEND_PLANES, [this](const char* name, size_t) {
			return new hybridacc::FIFO<noc_req_payload_t>(name, FIFO_DEPTH);
		});
		read_noc_addr_wait_fifo.init(NUM_SEND_PLANES, [this](const char* name, size_t) {
			return new hybridacc::FIFO<sc_uint<NOC_ADDR_BITS>>(name, FIFO_DEPTH);
		});

		// FIFOs port bindings for PS/PD/PLI planes
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			// NoC address tracking FIFO for read requests (encoded tag+ultra)
			read_noc_addr_wait_fifo[i].clk(clk);
			read_noc_addr_wait_fifo[i].reset_n(reset_n);
			read_noc_addr_wait_fifo[i].empty(read_noc_addr_wait_fifo_empty_out[i]);
			read_noc_addr_wait_fifo[i].full(read_noc_addr_wait_fifo_full_out[i]);
			read_noc_addr_wait_fifo[i].clear(read_noc_addr_wait_fifo_clear_sig[i]);
			read_noc_addr_wait_fifo[i].data_in(read_noc_addr_wait_fifo_data_in[i]);
			read_noc_addr_wait_fifo[i].data_out(read_noc_addr_wait_fifo_data_out[i]);
			read_noc_addr_wait_fifo[i].push(read_noc_addr_wait_fifo_push_in[i]);
			read_noc_addr_wait_fifo[i].pop(read_noc_addr_wait_fifo_pop_in[i]);

			// NoC req FIFOs
			noc_req_fifo[i].clk(clk);
			noc_req_fifo[i].reset_n(reset_n);
			noc_req_fifo[i].empty(noc_req_fifo_empty_out[i]);
			noc_req_fifo[i].full(noc_req_fifo_full_out[i]);
			noc_req_fifo[i].clear(noc_req_fifo_clear_sig[i]);
			noc_req_fifo[i].data_in(noc_req_fifo_data_in[i]);
			noc_req_fifo[i].data_out(noc_req_fifo_data_out[i]);
			noc_req_fifo[i].push(noc_req_fifo_push_in[i]);
			noc_req_fifo[i].pop(noc_req_fifo_pop_in[i]);
		}

		write_addr_fifo.clk(clk);
		write_addr_fifo.reset_n(reset_n);
		write_addr_fifo.empty(write_addr_fifo_empty_out);
		write_addr_fifo.full(write_addr_fifo_full_out);
		write_addr_fifo.clear(write_addr_fifo_clear_sig);
		write_addr_fifo.data_in(agu_gen_addr_sig[RECV_PLANE]); // Only PLO plane generates write addresses
		write_addr_fifo.data_out(write_addr_fifo_data_out);
		write_addr_fifo.push(write_addr_fifo_push_in);
		write_addr_fifo.pop(write_addr_fifo_pop_in);

		spm_req_fifo.clk(clk);
		spm_req_fifo.reset_n(reset_n);
		spm_req_fifo.empty(spm_req_fifo_empty_out);
		spm_req_fifo.full(spm_req_fifo_full_out);
		spm_req_fifo.clear(spm_req_fifo_clear_sig);
		spm_req_fifo.data_in(spm_req_fifo_data_in);
		spm_req_fifo.data_out(spm_req_fifo_data_out);
		spm_req_fifo.push(spm_req_fifo_push_in);
		spm_req_fifo.pop(spm_req_fifo_pop_in);

		// AGU instances and port bindings
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

		SC_METHOD(trace_process);
		sensitive << clk.pos();

		// --- Send Plane Logic (read from SPM, send to NoC) ---
		SC_METHOD(comb_spm_read_req);
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << agu_gen_valid_sig[i] << agu_gen_addr_sig[i] << plane_en_reg << read_noc_addr_wait_fifo_full_out[i];
		}

		SC_METHOD(comb_read_noc_addr_wait_fifo_data_in);
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << agu_gen_tag_sig[i] << agu_gen_ultra_sig[i];
		}

		SC_METHOD(comb_spm_read_req_push_wait_tag_fifo_agu_ready);
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << agu_gen_valid_sig[i] << plane_en_reg
					  << read_noc_addr_wait_fifo_full_out[i] << spm_req_ready[i];
		}

		SC_METHOD(comb_spm_read_resp_ready);
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << plane_en_reg << read_noc_addr_wait_fifo_empty_out[i] << noc_req_fifo_full_out[i];
		}

		SC_METHOD(comb_noc_send_req_fifo_data_in);
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << spm_resp_payload[i] << read_noc_addr_wait_fifo_data_out[i];
		}

		SC_METHOD(comb_spm_read_resp_pop_wait_tag_fifo_and_push_noc_req);
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << spm_resp_valid[i] << spm_resp_ready[i];
		}

		SC_METHOD(comb_noc_req_valid);
		sensitive << plane_en_reg;
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			sensitive << noc_req_fifo_data_out[i] << noc_req_fifo_empty_out[i];
		}

		SC_METHOD(comb_noc_req_pop);
		sensitive << plane_en_reg;
		sensitive << noc_req_fifo_empty_out[PLANE_PS] << noc_ps_out.ready_in;
		sensitive << noc_req_fifo_empty_out[PLANE_PD] << noc_pd_out.ready_in;
		sensitive << noc_req_fifo_empty_out[PLANE_PLI] << noc_pli_out.ready_in;

		// --- Receive Plane Logic (PLO: request from AGU, receive from NoC, write to SPM) ---
		SC_METHOD(comb_noc_plo_req_valid);
		sensitive << plane_en_reg << write_addr_fifo_full_out << agu_gen_valid_sig[RECV_PLANE] << agu_gen_tag_sig[RECV_PLANE] << agu_gen_ultra_sig[RECV_PLANE];

		SC_METHOD(comb_noc_plo_req_push_wait_addr_fifo_agu_ready);
		sensitive << plane_en_reg << write_addr_fifo_full_out << agu_gen_valid_sig[RECV_PLANE] << noc_plo_out.ready_in;

		SC_METHOD(comb_noc_plo_resp_ready);
		sensitive << plane_en_reg << write_addr_fifo_empty_out << spm_req_fifo_full_out;

		SC_METHOD(comb_spm_write_req_fifo_data_in);
		sensitive << write_addr_fifo_data_out << noc_plo_in.data_in;

		SC_METHOD(comb_noc_plo_resp_pop_wait_addr_fifo_and_push_spm_req);
		sensitive << noc_plo_in.valid_in << noc_plo_in.ready_out;

		SC_METHOD(comb_spm_write_req_valid);
		sensitive << plane_en_reg << spm_req_fifo_data_out << spm_req_fifo_empty_out;

		SC_METHOD(comb_spm_write_req_pop);
		sensitive << plane_en_reg << spm_req_fifo_empty_out << spm_req_ready[RECV_PLANE];
	}

	// --- Trace helper functions ---
	void set_trace_context(uint32_t pid, int tid_base) {
		trace_pid = pid;
		trace_id = tid_base;
		trace_init = false;
		last_state_core = "IDLE";
		last_state_tx = "IDLE";
		last_state_rx = "IDLE";
		last_state_spm = "IDLE";
		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			last_state_noc_req_fifo[i] = "IDLE";
			last_state_wait_tag_fifo[i] = "IDLE";
		}
		last_state_write_addr_fifo = "IDLE";
		last_state_spm_req_fifo = "IDLE";
	}

	int get_trace_num() const { return 13; }

	std::pair<uint32_t, uint32_t> enable_perffeto_trace(uint32_t start_pid, uint32_t start_tid) {
		set_trace_context(start_pid, static_cast<int>(start_tid));

		uint32_t next_pid = start_pid + 1;
		uint32_t next_tid = start_tid + static_cast<uint32_t>(get_trace_num() + 1);

		for (int i = 0; i < NUM_AGU; ++i) {
			agus[i].set_trace_context(next_pid, static_cast<int>(next_tid));
			next_tid += static_cast<uint32_t>(agus[i].get_trace_num() + 1);
		}
		next_pid += 1;
		return {next_pid, next_tid};
	}

private:
	static constexpr uint64_t DBG_REPORT_PERIOD = 256;

	int trace_id = -1;
	uint32_t trace_pid = 0;
	bool trace_init = false;
	std::string last_state_core = "IDLE";
	std::string last_state_tx = "IDLE";
	std::string last_state_rx = "IDLE";
	std::string last_state_spm = "IDLE";
	std::array<std::string, NUM_SEND_PLANES> last_state_noc_req_fifo{{"IDLE", "IDLE", "IDLE"}};
	std::array<std::string, NUM_SEND_PLANES> last_state_wait_tag_fifo{{"IDLE", "IDLE", "IDLE"}};
	std::string last_state_write_addr_fifo = "IDLE";
	std::string last_state_spm_req_fifo = "IDLE";

	void trace_process() {
		if (trace_id < 0) return;

		const uint32_t tid_core = static_cast<uint32_t>(trace_id + 1);
		const uint32_t tid_tx = static_cast<uint32_t>(trace_id + 2);
		const uint32_t tid_rx = static_cast<uint32_t>(trace_id + 3);
		const uint32_t tid_spm = static_cast<uint32_t>(trace_id + 4);
		const uint32_t tid_noc_req_fifo_ps = static_cast<uint32_t>(trace_id + 5);
		const uint32_t tid_noc_req_fifo_pd = static_cast<uint32_t>(trace_id + 6);
		const uint32_t tid_noc_req_fifo_pli = static_cast<uint32_t>(trace_id + 7);
		const uint32_t tid_wait_tag_fifo_ps = static_cast<uint32_t>(trace_id + 8);
		const uint32_t tid_wait_tag_fifo_pd = static_cast<uint32_t>(trace_id + 9);
		const uint32_t tid_wait_tag_fifo_pli = static_cast<uint32_t>(trace_id + 10);
		const uint32_t tid_write_addr_fifo = static_cast<uint32_t>(trace_id + 11);
		const uint32_t tid_spm_req_fifo = static_cast<uint32_t>(trace_id + 12);

		auto bool_to_json = [](bool v) { return v ? "true" : "false"; };
		auto fifo_state = [&](bool empty, bool full, bool push, bool pop) {
			if (push && pop) return std::string("PUSH_POP");
			if (push) return std::string("PUSH");
			if (pop) return std::string("POP");
			if (full) return std::string("FULL");
			if (empty) return std::string("EMPTY");
			return std::string("HAS_DATA");
		};
		auto trace_state = [&](std::string& last, const std::string& current,
							   const std::string& cat, uint32_t tid,
							   const std::string& args) {
			if (current != last) {
				TRACE_EVENT(last, cat, TRACE_END, trace_pid, tid, "{}");
				TRACE_EVENT(current, cat, TRACE_BEGIN, trace_pid, tid, args);
				last = current;
			}
		};

		if (!trace_init) {
			TRACE_THREAD_NAME(trace_pid, tid_core, std::string(name()) + " Core");
			TRACE_THREAD_NAME(trace_pid, tid_tx, std::string(name()) + " Tx");
			TRACE_THREAD_NAME(trace_pid, tid_rx, std::string(name()) + " Rx");
			TRACE_THREAD_NAME(trace_pid, tid_spm, std::string(name()) + " SPMHS");
			TRACE_THREAD_NAME(trace_pid, tid_noc_req_fifo_ps, std::string(name()) + " NOC_REQ_FIFO_PS");
			TRACE_THREAD_NAME(trace_pid, tid_noc_req_fifo_pd, std::string(name()) + " NOC_REQ_FIFO_PD");
			TRACE_THREAD_NAME(trace_pid, tid_noc_req_fifo_pli, std::string(name()) + " NOC_REQ_FIFO_PLI");
			TRACE_THREAD_NAME(trace_pid, tid_wait_tag_fifo_ps, std::string(name()) + " WAIT_TAG_FIFO_PS");
			TRACE_THREAD_NAME(trace_pid, tid_wait_tag_fifo_pd, std::string(name()) + " WAIT_TAG_FIFO_PD");
			TRACE_THREAD_NAME(trace_pid, tid_wait_tag_fifo_pli, std::string(name()) + " WAIT_TAG_FIFO_PLI");
			TRACE_THREAD_NAME(trace_pid, tid_write_addr_fifo, std::string(name()) + " WRITE_ADDR_FIFO");
			TRACE_THREAD_NAME(trace_pid, tid_spm_req_fifo, std::string(name()) + " SPM_REQ_FIFO");

			TRACE_EVENT(last_state_core, "HDDU_Core", TRACE_BEGIN, trace_pid, tid_core, "{}");
			TRACE_EVENT(last_state_tx, "HDDU_Tx", TRACE_BEGIN, trace_pid, tid_tx, "{}");
			TRACE_EVENT(last_state_rx, "HDDU_Rx", TRACE_BEGIN, trace_pid, tid_rx, "{}");
			TRACE_EVENT(last_state_spm, "HDDU_SPM_HS", TRACE_BEGIN, trace_pid, tid_spm, "{}");
			TRACE_EVENT(last_state_noc_req_fifo[PLANE_PS], "HDDU_FIFO_NOC_REQ_PS", TRACE_BEGIN, trace_pid, tid_noc_req_fifo_ps, "{}");
			TRACE_EVENT(last_state_noc_req_fifo[PLANE_PD], "HDDU_FIFO_NOC_REQ_PD", TRACE_BEGIN, trace_pid, tid_noc_req_fifo_pd, "{}");
			TRACE_EVENT(last_state_noc_req_fifo[PLANE_PLI], "HDDU_FIFO_NOC_REQ_PLI", TRACE_BEGIN, trace_pid, tid_noc_req_fifo_pli, "{}");
			TRACE_EVENT(last_state_wait_tag_fifo[PLANE_PS], "HDDU_FIFO_WAIT_TAG_PS", TRACE_BEGIN, trace_pid, tid_wait_tag_fifo_ps, "{}");
			TRACE_EVENT(last_state_wait_tag_fifo[PLANE_PD], "HDDU_FIFO_WAIT_TAG_PD", TRACE_BEGIN, trace_pid, tid_wait_tag_fifo_pd, "{}");
			TRACE_EVENT(last_state_wait_tag_fifo[PLANE_PLI], "HDDU_FIFO_WAIT_TAG_PLI", TRACE_BEGIN, trace_pid, tid_wait_tag_fifo_pli, "{}");
			TRACE_EVENT(last_state_write_addr_fifo, "HDDU_FIFO_WRITE_ADDR", TRACE_BEGIN, trace_pid, tid_write_addr_fifo, "{}");
			TRACE_EVENT(last_state_spm_req_fifo, "HDDU_FIFO_SPM_REQ", TRACE_BEGIN, trace_pid, tid_spm_req_fifo, "{}");
			trace_init = true;
		}

		bool any_busy = false;
		bool any_gen_valid = false;
		for (int i = 0; i < NUM_AGU; ++i) {
			any_busy = any_busy || agu_busy_sig[i].read();
			any_gen_valid = any_gen_valid || agu_gen_valid_sig[i].read();
		}
		const bool has_error = (err_code_reg.read() != 0);
		const std::string core_state = has_error ? "ERROR" : (any_busy ? "RUN" : "IDLE");
		trace_state(last_state_core, core_state, "HDDU_Core", tid_core,
					std::string("{\"busy\": ") + bool_to_json(any_busy)
					+ ", \"gen_valid\": " + bool_to_json(any_gen_valid)
					+ ", \"error\": " + bool_to_json(has_error) + "}");

		const bool tx_active = noc_ps_out.valid_out.read() || noc_pd_out.valid_out.read() || noc_pli_out.valid_out.read();
		const bool tx_ready = (noc_ps_out.valid_out.read() && noc_ps_out.ready_in.read())
							  || (noc_pd_out.valid_out.read() && noc_pd_out.ready_in.read())
							  || (noc_pli_out.valid_out.read() && noc_pli_out.ready_in.read());
		const std::string noc_req_fifo_ps_state = fifo_state(
			noc_req_fifo_empty_out[PLANE_PS].read(),
			noc_req_fifo_full_out[PLANE_PS].read(),
			noc_req_fifo_push_in[PLANE_PS].read(),
			noc_req_fifo_pop_in[PLANE_PS].read());
		const std::string noc_req_fifo_pd_state = fifo_state(
			noc_req_fifo_empty_out[PLANE_PD].read(),
			noc_req_fifo_full_out[PLANE_PD].read(),
			noc_req_fifo_push_in[PLANE_PD].read(),
			noc_req_fifo_pop_in[PLANE_PD].read());
		const std::string noc_req_fifo_pli_state = fifo_state(
			noc_req_fifo_empty_out[PLANE_PLI].read(),
			noc_req_fifo_full_out[PLANE_PLI].read(),
			noc_req_fifo_push_in[PLANE_PLI].read(),
			noc_req_fifo_pop_in[PLANE_PLI].read());
		const std::string wait_tag_fifo_ps_state = fifo_state(
			read_noc_addr_wait_fifo_empty_out[PLANE_PS].read(),
			read_noc_addr_wait_fifo_full_out[PLANE_PS].read(),
			read_noc_addr_wait_fifo_push_in[PLANE_PS].read(),
			read_noc_addr_wait_fifo_pop_in[PLANE_PS].read());
		const std::string wait_tag_fifo_pd_state = fifo_state(
			read_noc_addr_wait_fifo_empty_out[PLANE_PD].read(),
			read_noc_addr_wait_fifo_full_out[PLANE_PD].read(),
			read_noc_addr_wait_fifo_push_in[PLANE_PD].read(),
			read_noc_addr_wait_fifo_pop_in[PLANE_PD].read());
		const std::string wait_tag_fifo_pli_state = fifo_state(
			read_noc_addr_wait_fifo_empty_out[PLANE_PLI].read(),
			read_noc_addr_wait_fifo_full_out[PLANE_PLI].read(),
			read_noc_addr_wait_fifo_push_in[PLANE_PLI].read(),
			read_noc_addr_wait_fifo_pop_in[PLANE_PLI].read());
		trace_state(last_state_noc_req_fifo[PLANE_PS], noc_req_fifo_ps_state, "HDDU_FIFO_NOC_REQ_PS", tid_noc_req_fifo_ps,
					std::string("{\"empty\": ") + bool_to_json(noc_req_fifo_empty_out[PLANE_PS].read())
					+ ", \"full\": " + bool_to_json(noc_req_fifo_full_out[PLANE_PS].read())
					+ ", \"push\": " + bool_to_json(noc_req_fifo_push_in[PLANE_PS].read())
					+ ", \"pop\": " + bool_to_json(noc_req_fifo_pop_in[PLANE_PS].read()) + "}");
		trace_state(last_state_noc_req_fifo[PLANE_PD], noc_req_fifo_pd_state, "HDDU_FIFO_NOC_REQ_PD", tid_noc_req_fifo_pd,
					std::string("{\"empty\": ") + bool_to_json(noc_req_fifo_empty_out[PLANE_PD].read())
					+ ", \"full\": " + bool_to_json(noc_req_fifo_full_out[PLANE_PD].read())
					+ ", \"push\": " + bool_to_json(noc_req_fifo_push_in[PLANE_PD].read())
					+ ", \"pop\": " + bool_to_json(noc_req_fifo_pop_in[PLANE_PD].read()) + "}");
		trace_state(last_state_noc_req_fifo[PLANE_PLI], noc_req_fifo_pli_state, "HDDU_FIFO_NOC_REQ_PLI", tid_noc_req_fifo_pli,
					std::string("{\"empty\": ") + bool_to_json(noc_req_fifo_empty_out[PLANE_PLI].read())
					+ ", \"full\": " + bool_to_json(noc_req_fifo_full_out[PLANE_PLI].read())
					+ ", \"push\": " + bool_to_json(noc_req_fifo_push_in[PLANE_PLI].read())
					+ ", \"pop\": " + bool_to_json(noc_req_fifo_pop_in[PLANE_PLI].read()) + "}");
		trace_state(last_state_wait_tag_fifo[PLANE_PS], wait_tag_fifo_ps_state, "HDDU_FIFO_WAIT_TAG_PS", tid_wait_tag_fifo_ps,
					std::string("{\"empty\": ") + bool_to_json(read_noc_addr_wait_fifo_empty_out[PLANE_PS].read())
					+ ", \"full\": " + bool_to_json(read_noc_addr_wait_fifo_full_out[PLANE_PS].read())
					+ ", \"push\": " + bool_to_json(read_noc_addr_wait_fifo_push_in[PLANE_PS].read())
					+ ", \"pop\": " + bool_to_json(read_noc_addr_wait_fifo_pop_in[PLANE_PS].read()) + "}");
		trace_state(last_state_wait_tag_fifo[PLANE_PD], wait_tag_fifo_pd_state, "HDDU_FIFO_WAIT_TAG_PD", tid_wait_tag_fifo_pd,
					std::string("{\"empty\": ") + bool_to_json(read_noc_addr_wait_fifo_empty_out[PLANE_PD].read())
					+ ", \"full\": " + bool_to_json(read_noc_addr_wait_fifo_full_out[PLANE_PD].read())
					+ ", \"push\": " + bool_to_json(read_noc_addr_wait_fifo_push_in[PLANE_PD].read())
					+ ", \"pop\": " + bool_to_json(read_noc_addr_wait_fifo_pop_in[PLANE_PD].read()) + "}");
		trace_state(last_state_wait_tag_fifo[PLANE_PLI], wait_tag_fifo_pli_state, "HDDU_FIFO_WAIT_TAG_PLI", tid_wait_tag_fifo_pli,
					std::string("{\"empty\": ") + bool_to_json(read_noc_addr_wait_fifo_empty_out[PLANE_PLI].read())
					+ ", \"full\": " + bool_to_json(read_noc_addr_wait_fifo_full_out[PLANE_PLI].read())
					+ ", \"push\": " + bool_to_json(read_noc_addr_wait_fifo_push_in[PLANE_PLI].read())
					+ ", \"pop\": " + bool_to_json(read_noc_addr_wait_fifo_pop_in[PLANE_PLI].read()) + "}");
		const std::string tx_state = tx_active ? (tx_ready ? "XFER" : "WAIT_READY") : "IDLE";
		trace_state(last_state_tx, tx_state, "HDDU_Tx", tid_tx,
					std::string("{\"active\": ") + bool_to_json(tx_active)
					+ ", \"ready\": " + bool_to_json(tx_ready)
					+ ", \"noc_req_fifo_ps\": \"" + noc_req_fifo_ps_state + "\""
					+ ", \"noc_req_fifo_pd\": \"" + noc_req_fifo_pd_state + "\""
					+ ", \"noc_req_fifo_pli\": \"" + noc_req_fifo_pli_state + "\""
					+ ", \"wait_tag_fifo_ps\": \"" + wait_tag_fifo_ps_state + "\""
					+ ", \"wait_tag_fifo_pd\": \"" + wait_tag_fifo_pd_state + "\""
					+ ", \"wait_tag_fifo_pli\": \"" + wait_tag_fifo_pli_state + "\""
					+ "}");

		const bool rx_valid = noc_plo_in.valid_in.read();
		const bool rx_ready = noc_plo_in.ready_out.read();
		const std::string write_addr_fifo_state = fifo_state(
			write_addr_fifo_empty_out.read(),
			write_addr_fifo_full_out.read(),
			write_addr_fifo_push_in.read(),
			write_addr_fifo_pop_in.read());
		const std::string spm_req_fifo_state = fifo_state(
			spm_req_fifo_empty_out.read(),
			spm_req_fifo_full_out.read(),
			spm_req_fifo_push_in.read(),
			spm_req_fifo_pop_in.read());
		trace_state(last_state_write_addr_fifo, write_addr_fifo_state, "HDDU_FIFO_WRITE_ADDR", tid_write_addr_fifo,
					std::string("{\"empty\": ") + bool_to_json(write_addr_fifo_empty_out.read())
					+ ", \"full\": " + bool_to_json(write_addr_fifo_full_out.read())
					+ ", \"push\": " + bool_to_json(write_addr_fifo_push_in.read())
					+ ", \"pop\": " + bool_to_json(write_addr_fifo_pop_in.read()) + "}");
		trace_state(last_state_spm_req_fifo, spm_req_fifo_state, "HDDU_FIFO_SPM_REQ", tid_spm_req_fifo,
					std::string("{\"empty\": ") + bool_to_json(spm_req_fifo_empty_out.read())
					+ ", \"full\": " + bool_to_json(spm_req_fifo_full_out.read())
					+ ", \"push\": " + bool_to_json(spm_req_fifo_push_in.read())
					+ ", \"pop\": " + bool_to_json(spm_req_fifo_pop_in.read()) + "}");
		const std::string rx_state = rx_valid ? (rx_ready ? "XFER" : "WAIT_ADDR") : "IDLE";
		trace_state(last_state_rx, rx_state, "HDDU_Rx", tid_rx,
					std::string("{\"valid\": ") + bool_to_json(rx_valid)
					+ ", \"ready\": " + bool_to_json(rx_ready)
					+ ", \"pending_addr\": " + (write_addr_fifo_empty_out.read() ? std::string("0") : std::string("1+"))
					+ ", \"write_addr_fifo\": \"" + write_addr_fifo_state + "\""
					+ ", \"spm_req_fifo\": \"" + spm_req_fifo_state + "\""
					+ "}");

		bool any_spm_req = false;
		bool any_spm_ready = false;
		for (int i = 0; i < NUM_SPM; ++i) {
			any_spm_req = any_spm_req || spm_req_valid[i].read();
			any_spm_ready = any_spm_ready || spm_req_ready[i].read();

			if (spm_req_valid[i].read() && spm_req_ready[i].read()) {
				DEBUG_MSG("SPM_REQ_FIRE plane=" << i
						  << " addr=0x" << std::hex << agu_gen_addr_sig[i].read().to_uint()
						  << " tag=0x" << std::hex << agu_gen_tag_sig[i].read().to_uint()
						  << std::dec,
						  DEBUG_LEVEL_CLUSTER_COMPONENTS);
			}
		}

		const bool any_spm_fire = any_spm_req && any_spm_ready;
		const std::string spm_state = any_spm_req ? (any_spm_fire ? "XFER" : "WAIT_READY") : "IDLE";
		const uint32_t addr_ps = agu_gen_addr_sig[PLANE_PS].read().to_uint();
		const uint32_t addr_pd = agu_gen_addr_sig[PLANE_PD].read().to_uint();
		const uint32_t addr_pli = agu_gen_addr_sig[PLANE_PLI].read().to_uint();
		const uint32_t addr_plo = agu_gen_addr_sig[PLANE_PLO].read().to_uint();
		const uint32_t tag_ps = agu_gen_tag_sig[PLANE_PS].read().to_uint();
		const uint32_t tag_pd = agu_gen_tag_sig[PLANE_PD].read().to_uint();
		const uint32_t tag_pli = agu_gen_tag_sig[PLANE_PLI].read().to_uint();
		const uint32_t tag_plo = agu_gen_tag_sig[PLANE_PLO].read().to_uint();
		trace_state(last_state_spm, spm_state, "HDDU_SPM_HS", tid_spm,
					std::string("{\"agu_valid\": ") + bool_to_json(any_gen_valid)
					+ ", \"req\": " + bool_to_json(any_spm_req)
					+ ", \"ready\": " + bool_to_json(any_spm_ready)
					+ ", \"fire\": " + bool_to_json(any_spm_fire)
					+ ", \"write_addr_fifo\": \"" + write_addr_fifo_state + "\""
					+ ", \"spm_req_fifo\": \"" + spm_req_fifo_state + "\""
					+ ", \"addr_ps\": " + std::to_string(addr_ps)
					+ ", \"tag_ps\": " + std::to_string(tag_ps)
					+ ", \"addr_pd\": " + std::to_string(addr_pd)
					+ ", \"tag_pd\": " + std::to_string(tag_pd)
					+ ", \"addr_pli\": " + std::to_string(addr_pli)
					+ ", \"tag_pli\": " + std::to_string(tag_pli)
					+ ", \"addr_plo\": " + std::to_string(addr_plo)
					+ ", \"tag_plo\": " + std::to_string(tag_plo) + "}");
	}

	// --- Internal state and helper functions ---
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

	// Internal FIFO wires
	sc_vector<sc_signal<bool>> noc_req_fifo_empty_out;
	sc_vector<sc_signal<bool>> noc_req_fifo_full_out;
	sc_vector<sc_signal<noc_req_payload_t>> noc_req_fifo_data_out;
	sc_vector<sc_signal<noc_req_payload_t>> noc_req_fifo_data_in;
	sc_vector<sc_signal<bool>> noc_req_fifo_push_in;
	sc_vector<sc_signal<bool>> noc_req_fifo_pop_in;
	sc_vector<sc_signal<bool>> noc_req_fifo_clear_sig;

	sc_vector<sc_signal<bool>> read_noc_addr_wait_fifo_empty_out;
	sc_vector<sc_signal<bool>> read_noc_addr_wait_fifo_full_out;
	sc_vector<sc_signal<sc_uint<NOC_ADDR_BITS>>> read_noc_addr_wait_fifo_data_out;
	sc_vector<sc_signal<sc_uint<NOC_ADDR_BITS>>> read_noc_addr_wait_fifo_data_in;
	sc_vector<sc_signal<bool>> read_noc_addr_wait_fifo_push_in;
	sc_vector<sc_signal<bool>> read_noc_addr_wait_fifo_pop_in;
	sc_vector<sc_signal<bool>> read_noc_addr_wait_fifo_clear_sig;

	sc_signal<bool> write_addr_fifo_empty_out;
	sc_signal<bool> write_addr_fifo_full_out;
	sc_signal<sc_uint<SPM_ADDR_BITS>> write_addr_fifo_data_out;
	sc_signal<sc_uint<SPM_ADDR_BITS>> write_addr_fifo_data_in;
	sc_signal<bool> write_addr_fifo_push_in;
	sc_signal<bool> write_addr_fifo_pop_in;
	sc_signal<bool> write_addr_fifo_clear_sig;

	sc_signal<bool> spm_req_fifo_empty_out;
	sc_signal<bool> spm_req_fifo_full_out;
	sc_signal<spm_req_payload_t> spm_req_fifo_data_out;
	sc_signal<spm_req_payload_t> spm_req_fifo_data_in;
	sc_signal<bool> spm_req_fifo_push_in;
	sc_signal<bool> spm_req_fifo_pop_in;
	sc_signal<bool> spm_req_fifo_clear_sig;

	// Internal registers
	sc_signal<sc_uint<32>> global_ctrl_reg;
	sc_signal<sc_uint<32>> global_status_reg;
	sc_signal<sc_uint<32>> plane_en_reg;
	sc_signal<sc_uint<32>> plane_mode_reg;
	sc_signal<sc_uint<32>> arb_policy_reg;
	sc_signal<sc_uint<32>> err_code_reg;
	sc_signal<sc_uint<32>> err_info0_reg;
	sc_signal<sc_uint<32>> err_info1_reg;
	sc_signal<sc_uint<32>> counter_tx_pkt_reg;
	sc_signal<sc_uint<32>> counter_tx_byte_reg;
	sc_signal<sc_uint<32>> counter_rx_byte_reg;
	sc_signal<sc_uint<32>> counter_stall_reg;

	// Latched state for DONE generation and run tracking.
	bool run_active_latched = false;
	bool prev_any_busy_latched = false;
	bool done_latched = false;
	bool start_pending_ = false;  // plain C++ var for immediate BUSY visibility

	// Internal FIFOs for each NoC channel
	sc_vector<hybridacc::FIFO<noc_req_payload_t>> noc_req_fifo; // De-couple AGU request generation and NoC transmission for PS/PD/PLI planes
	sc_vector<hybridacc::FIFO<sc_uint<NOC_ADDR_BITS>>> read_noc_addr_wait_fifo; // Track encoded NoC addr(tag+ultra) for in-flight read requests for PS/PD/PLI planes

	hybridacc::FIFO<sc_uint<SPM_ADDR_BITS>> write_addr_fifo; // Track the write addresses for NoC in-flight transactions for PLO plane
	hybridacc::FIFO<spm_req_payload_t> spm_req_fifo; // Buffer SPM requests for bypassing logic

	// Helper function to check if an address is within AGU MMIO range
	static bool is_agu_mmio_addr(uint32_t addr) {
		return addr >= MMIO_AGU_BASE && addr < (MMIO_AGU_BASE + MMIO_AGU_SIZE);
	}

	// Helper function to check if an address is within global MMIO range
	static bool is_global_mmio_addr(uint32_t addr) {
		return addr >= MMIO_GLOBAL_BASE && addr <= MMIO_GLOBAL_END;
	}

	// Helper function to encode NoC address for PLO plane based on tag and ultra mode
	static sc_uint<NOC_ADDR_BITS> encode_noc_addr(sc_uint<16> tag, bool ultra) {
		sc_uint<NOC_ADDR_BITS> value = 0;
		const sc_uint<NOC_TAG_BITS> tag_n = static_cast<sc_uint<NOC_TAG_BITS>>(tag);
		value.range(NOC_TAG_BITS - 1, 0) = tag_n;
		value[NOC_TAG_BITS] = ultra ? 1 : 0;
		return value;
	}

	// Helper function to convert NoC tag to SPM tag (for PS/PD/PLI planes)
	static noc_req_payload_t make_noc_req(sc_biguint<DATA_BITS> data, sc_uint<NOC_ADDR_BITS> addr) {
		noc_req_payload_t req{};
		req.data = data;
		req.addr = static_cast<uint16_t>(addr.to_uint());
		req.mask = static_cast<size_t>(~0ULL);
		return req;
	}

	// Helper function to convert NoC address payload to SPM address payload (for PLO plane)
	static noc_addr_payload_t make_noc_plo_req(sc_uint<16> tag, bool ultra) {
		noc_addr_payload_t req{};
		sc_uint<NOC_ADDR_BITS> addr = 0;
		const sc_uint<NOC_TAG_BITS> tag_n = static_cast<sc_uint<NOC_TAG_BITS>>(tag);
		addr.range(NOC_TAG_BITS - 1, 0) = tag_n;
		addr[NOC_TAG_BITS] = ultra ? 1 : 0;
		req.addr = static_cast<uint16_t>(addr.to_uint());
		return req;
	}

	// Helper function to reset internal state
	void reset_internal() {
		global_ctrl_reg.write(0);
		global_status_reg.write(0);
		plane_en_reg.write(DEFAULT_PLANE_EN);
		plane_mode_reg.write(0);
		arb_policy_reg.write(0);
		err_code_reg.write(0);
		err_info0_reg.write(0);
		err_info1_reg.write(0);
		counter_tx_pkt_reg.write(0);
		counter_tx_byte_reg.write(0);
		counter_rx_byte_reg.write(0);
		counter_stall_reg.write(0);
		interrupt.write(false);
		arb_state.write(0);
		run_active_latched = false;
		prev_any_busy_latched = false;
		done_latched = false;
		start_pending_ = false;

		for (int i = 0; i < NUM_SEND_PLANES; ++i) {
			noc_req_fifo_clear_sig[i].write(false);
			read_noc_addr_wait_fifo_clear_sig[i].write(false);
		}
		write_addr_fifo_clear_sig.write(false);
		spm_req_fifo_clear_sig.write(false);

		for (int i = 0; i < NUM_AGU; ++i) {
			agu_cfg_write_sig[i].write(false);
			agu_cfg_addr_sig[i].write(0);
			agu_cfg_wdata_sig[i].write(0);
			agu_start_sig[i].write(false);
			agu_stop_sig[i].write(false);
		}
	}

	// Helper function to route MMIO writes to the appropriate AGU configuration signals
	void route_mmio_to_agu(sc_uint<32> addr, sc_uint<32> wdata, bool is_write) {
		const uint32_t a = addr.to_uint();
		if (!is_agu_mmio_addr(a)) {
			for (int i = 0; i < NUM_AGU; ++i) {
				agu_cfg_write_sig[i].write(false);
				agu_start_sig[i].write(false);
				agu_stop_sig[i].write(false);
			}
			return;
		}

		const int bank = static_cast<int>((a >> 8) & MMIO_AGU_BANK_MASK);
		const sc_uint<8> sub = static_cast<sc_uint<8>>(a & MMIO_AGU_SUB_MASK);

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

		if (sub == MMIO_AGU_CTRL_OFFSET) {
			if (wdata[0]) agu_start_sig[bank].write(true);
			if (wdata[1]) agu_stop_sig[bank].write(true);
		}

		agu_cfg_write_sig[bank].write(true);
	}

	// Helper function to handle global MMIO writes
	void handle_global_mmio_write(sc_uint<32> addr, sc_uint<32> wdata) {
		const uint32_t a = addr.to_uint();
		if (!is_global_mmio_addr(a)) {
			return;
		}

		switch (a) {
			case MMIO_GLOBAL_CTRL: {
				global_ctrl_reg.write(wdata);
				if (wdata[(int)HdduCtrllBit::CTRL_RESET]) { // Global reset bit
					err_code_reg.write(0);
					err_info0_reg.write(0);
					err_info1_reg.write(0);

					// Clear all FIFOs
					for (int i = 0; i < NUM_SEND_PLANES; ++i) {
						noc_req_fifo_clear_sig[i].write(true);
						read_noc_addr_wait_fifo_clear_sig[i].write(true);
					}
					write_addr_fifo_clear_sig.write(true);
					spm_req_fifo_clear_sig.write(true);
				}
				if (wdata[(int)HdduCtrllBit::CTRL_START]) { // Global start bit
					for (int i = 0; i < NUM_AGU; ++i) {
						agu_start_sig[i].write(true);
					}
					// Immediately reflect BUSY in status so firmware polling
					// doesn't race with the 1-cycle AGU startup delay.
					sc_uint<32> status = global_status_reg.read();
					status[(int)HdduStatusBit::BUSY] = true;
					status[(int)HdduStatusBit::IDLE] = false;
					global_status_reg.write(status);
					start_pending_ = true;  // visible to comb_mmio_read immediately
				}
				if (wdata[(int)HdduCtrllBit::CTRL_STOP]) { // Global stop bit
					for (int i = 0; i < NUM_AGU; ++i) {
						agu_stop_sig[i].write(true);
					}
				}
				break;
			}
			case MMIO_GLOBAL_PLANE_EN: {
				plane_en_reg.write(wdata); break;
			}
			case MMIO_GLOBAL_PLANE_MODE: {
				plane_mode_reg.write(wdata); break;
			}
			case MMIO_GLOBAL_ARB_POLICY: arb_policy_reg.write(wdata); break;
			default: break;
		}
	}

	// Combinational logic to handle MMIO reads and return the appropriate data based on the address
	void comb_mmio_read() {
		const uint32_t a = mmio_addr.read().to_uint();
		sc_uint<32> r = 0;

		if (is_agu_mmio_addr(a)) {
			int bank = static_cast<int>((a >> 8) & MMIO_AGU_BANK_MASK);
			if (bank >= 0 && bank < NUM_AGU) {
				r = agu_cfg_rdata_sig[bank].read();
			}
		} else if (is_global_mmio_addr(a)) {
			switch (a) {
				case MMIO_GLOBAL_CTRL: r = global_ctrl_reg.read(); break;
				case MMIO_GLOBAL_STATUS: {
					r = global_status_reg.read();
					// Use plain C++ flag for same-cycle BUSY visibility
					if (start_pending_) {
						r[(int)HdduStatusBit::BUSY] = true;
						r[(int)HdduStatusBit::IDLE] = false;
					}
					break;
				}
				case MMIO_GLOBAL_PLANE_EN: r = plane_en_reg.read(); break;
				case MMIO_GLOBAL_PLANE_MODE: r = plane_mode_reg.read(); break;
				case MMIO_GLOBAL_NUM_PLANES: r = NUM_AGU; break;
				case MMIO_GLOBAL_PORT_WIDTH: r = DATA_BITS / NUM_NOC; break;
				case MMIO_GLOBAL_ARB_POLICY: r = arb_policy_reg.read(); break;
				case MMIO_GLOBAL_ERR_CODE: r = err_code_reg.read(); break;
				case MMIO_GLOBAL_ERR_INFO0: r = err_info0_reg.read(); break;
				case MMIO_GLOBAL_ERR_INFO1: r = err_info1_reg.read(); break;
				case MMIO_GLOBAL_COUNTER_TX_PKT: r = counter_tx_pkt_reg.read(); break;
				case MMIO_GLOBAL_COUNTER_TX_BYTE: r = counter_tx_byte_reg.read(); break;
				case MMIO_GLOBAL_COUNTER_RX_BYTE: r = counter_rx_byte_reg.read(); break;
				case MMIO_GLOBAL_COUNTER_STALL: r = counter_stall_reg.read(); break;
				default: r = 0; break;
			}
		}

		mmio_rdata.write(r);
	}

	/////////////////// Send plane combinational logic ///////////////////

	void comb_read_noc_addr_wait_fifo_data_in() {
		for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
			read_noc_addr_wait_fifo_data_in[lane].write(
				encode_noc_addr(agu_gen_tag_sig[lane].read(), agu_gen_ultra_sig[lane].read()));
		}
	}

	// Combinational logic to bypass AGU-generated requests directly to SPM if the corresponding plane is enabled and there is room in the FIFO, to reduce latency
	void comb_spm_read_req(){
		for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
			const bool valid = agu_gen_valid_sig[lane].read();
			const bool plane_enabled = ((plane_en_reg.read().to_uint() >> lane) & 0x1u) != 0;
			const bool room_in_fifo = !read_noc_addr_wait_fifo_full_out[lane].read();
			if (valid && plane_enabled && room_in_fifo) {
				spm_req_payload_t payload{};
				payload.addr = agu_gen_addr_sig[lane].read();
				payload.wdata = 0; // data is don't care for read requests
				payload.wen = false;

				spm_req_payload[lane].write(payload);
				spm_req_valid[lane].write(true);
			} else {
				spm_req_payload[lane].write(spm_req_payload_t{}); // default payload
				spm_req_valid[lane].write(false);
			}
		}
	}

	// Combinational logic to push the generated tag from AGU into the wait FIFO and assert ready back to AGU when an SPM read request is accepted, for each plane if enabled, to track the in-flight read requests and their tags
	void comb_spm_read_req_push_wait_tag_fifo_agu_ready(){
		for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
			const bool valid = agu_gen_valid_sig[lane].read()
						&& (((plane_en_reg.read().to_uint() >> lane) & 0x1u) != 0)
						&& !read_noc_addr_wait_fifo_full_out[lane].read();
			const bool ready = spm_req_ready[lane].read();
			if (valid && ready) {
				read_noc_addr_wait_fifo_push_in[lane].write(true);
				agu_gen_ready_sig[lane].write(true); // also assert ready to AGU to complete the handshake
			} else {
				read_noc_addr_wait_fifo_push_in[lane].write(false);
				agu_gen_ready_sig[lane].write(false);
			}
		}
	}

	// Combinational logic to drive the SPM response ready signal based on whether the corresponding plane is enabled, there is a pending tag in the wait FIFO, and there is room in the NoC request FIFO to accept a new request
	void comb_spm_read_resp_ready(){
		for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
			const bool plane_enabled = ((plane_en_reg.read().to_uint() >> lane) & 0x1u) != 0;
			const bool waiting_for_resp = !read_noc_addr_wait_fifo_empty_out[lane].read(); // resp in flight if there is a tag in the wait FIFO
			const bool room_in_noc_fifo = !noc_req_fifo_full_out[lane].read(); // can accept new NoC req if there is room in the FIFO

			if (plane_enabled && waiting_for_resp && room_in_noc_fifo) {
				spm_resp_ready[lane].write(true);
			} else {
				spm_resp_ready[lane].write(false);
			}

		}

		// For PLO plane (write response), accept response as long as plane is enabled
		const bool plane_on_plo = ((plane_en_reg.read().to_uint() >> PLANE_PLO) & 0x1u) != 0;
		const bool waiting_for_wr_resp = !write_addr_fifo_empty_out.read() || !spm_req_fifo_empty_out.read();
		spm_resp_ready[PLANE_PLO].write(plane_on_plo && waiting_for_wr_resp);
	}

	// Combinational logic to combine the SPM response data with the corresponding tag from the wait FIFO to form the NoC request payload, for each plane if enabled
	void comb_noc_send_req_fifo_data_in(){
		// read the tag from the head of the wait FIFO and combine with the SPM response data to form the NoC request payload
		for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
			spm_resp_payload_t resp = spm_resp_payload[lane].read();
			const sc_uint<NOC_ADDR_BITS> noc_addr = read_noc_addr_wait_fifo_data_out[lane].read();
			noc_req_payload_t req = make_noc_req(resp.rdata, noc_addr);
			noc_req_fifo_data_in[lane].write(req);
		}
	}

	// Combinational logic to pop the tag tracking FIFO and push the NoC request FIFO when an SPM response is accepted, for each plane if enabled
	void comb_spm_read_resp_pop_wait_tag_fifo_and_push_noc_req(){
		for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
			const bool valid = spm_resp_valid[lane].read();
			const bool ready = spm_resp_ready[lane].read();
			if (valid && ready) {
				read_noc_addr_wait_fifo_pop_in[lane].write(true);
				noc_req_fifo_push_in[lane].write(true);
			} else {
				read_noc_addr_wait_fifo_pop_in[lane].write(false);
				noc_req_fifo_push_in[lane].write(false);
			}
		}
	}

	// Combinational logic to drive the NoC request outputs based on the corresponding FIFOs for each plane, and whether the plane is enabled
	void comb_noc_req_valid(){
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_ps = ((plane_en >> PLANE_PS) & 0x1u) != 0;
		const bool plane_on_pd = ((plane_en >> PLANE_PD) & 0x1u) != 0;
		const bool plane_on_pli = ((plane_en >> PLANE_PLI) & 0x1u) != 0;

		auto run_noc_send_req = [&](int lane, VRDOF<noc_req_payload_t>& out_if, bool plane_enabled) {
			if (plane_enabled) {
				out_if.data_out.write(noc_req_fifo_data_out[lane].read());
				out_if.valid_out.write(!noc_req_fifo_empty_out[lane].read());
			} else {
				out_if.data_out.write(noc_req_payload_t{});
				out_if.valid_out.write(false);
			}
		};

		run_noc_send_req(PLANE_PS, noc_ps_out, plane_on_ps);
		run_noc_send_req(PLANE_PD, noc_pd_out, plane_on_pd);
		run_noc_send_req(PLANE_PLI, noc_pli_out, plane_on_pli);
	}

	// Combinational logic to pop the NoC request FIFOs when the corresponding NoC output interface handshakes, for each plane if enabled
	void comb_noc_req_pop(){
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_ps = ((plane_en >> PLANE_PS) & 0x1u) != 0;
		const bool plane_on_pd = ((plane_en >> PLANE_PD) & 0x1u) != 0;
		const bool plane_on_pli = ((plane_en >> PLANE_PLI) & 0x1u) != 0;

		auto pop_noc_send_req = [&](int lane, VRDOF<noc_req_payload_t>& out_if, bool plane_enabled) {
			bool valid = !noc_req_fifo_empty_out[lane].read();
			bool ready = out_if.ready_in.read();
			if (plane_enabled && valid && ready) {
				noc_req_fifo_pop_in[lane].write(true);
			} else {
				noc_req_fifo_pop_in[lane].write(false);
			}
		};

		pop_noc_send_req(PLANE_PS, noc_ps_out, plane_on_ps);
		pop_noc_send_req(PLANE_PD, noc_pd_out, plane_on_pd);
		pop_noc_send_req(PLANE_PLI, noc_pli_out, plane_on_pli);

	}

	/////////////////// Receive plane combinational logic ///////////////////

	// Combinational logic to drive the NoC request output for PLO plane based on the AGU-generated tag and ultra signals, if the plane is enabled, to bypass the FIFO and reduce latency since PLO plane is performance critical
	void comb_noc_plo_req_valid(){
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;
		const bool room_in_addr_fifo = !write_addr_fifo_full_out.read();
		const bool req_can_issue = plane_on_plo && room_in_addr_fifo;

		if (req_can_issue) {
			noc_plo_out.valid_out.write(agu_gen_valid_sig[RECV_PLANE].read());
			noc_plo_out.data_out.write(make_noc_plo_req(agu_gen_tag_sig[RECV_PLANE].read(), agu_gen_ultra_sig[RECV_PLANE].read()));
		} else {
			noc_plo_out.valid_out.write(false);
			noc_plo_out.data_out.write(noc_addr_payload_t{});
		}
	}

	// Combinational logic to assert ready for AGU-generated requests for PLO plane when the NoC output handshakes, to complete the decoupled handshake and allow the AGU to push the tag into the FIFO to track the in-flight transaction
	void comb_noc_plo_req_push_wait_addr_fifo_agu_ready(){
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;
		const bool room_in_addr_fifo = !write_addr_fifo_full_out.read();
		const bool valid = agu_gen_valid_sig[RECV_PLANE].read();
		const bool ready = noc_plo_out.ready_in.read();

		if (plane_on_plo && room_in_addr_fifo && valid && ready) {
			write_addr_fifo_push_in.write(true);
			agu_gen_ready_sig[RECV_PLANE].write(true); // also assert ready to AGU to complete the handshake
		} else {
			write_addr_fifo_push_in.write(false);
			agu_gen_ready_sig[RECV_PLANE].write(false);
		}
	}

	// Combinational logic to drive the NoC response ready signal for PLO plane based on whether the plane is enabled, there is a pending address in the wait FIFO, and there is room in the SPM request FIFO to accept a new request, to allow accepting responses from NoC and pushing new requests to SPM for bypassing logic
	void comb_noc_plo_resp_ready(){
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;
		const bool waiting_for_resp = !write_addr_fifo_empty_out.read();
		const bool room_in_noc_fifo = !spm_req_fifo_full_out.read();

		if (plane_on_plo && waiting_for_resp && room_in_noc_fifo) {
			noc_plo_in.ready_out.write(true);
		} else {
			noc_plo_in.ready_out.write(false);
		}
	}

	// Combinational logic to combine the NoC response data with the corresponding address from the wait FIFO to form the SPM request payload for PLO plane, to push to the SPM request FIFO for bypassing logic
	void comb_spm_write_req_fifo_data_in() {
		spm_req_payload_t payload{};
		payload.addr = write_addr_fifo_data_out.read();
		payload.wdata = noc_plo_in.data_in.read().data;
		payload.wen = true;
		spm_req_fifo_data_in.write(payload);
	}

	// Combinational logic to pop the address tracking FIFO and push the SPM request FIFO when a NoC response is accepted for PLO plane, to track the in-flight transaction and push the corresponding request to SPM for bypassing logic
	void comb_noc_plo_resp_pop_wait_addr_fifo_and_push_spm_req() {
		const bool valid = noc_plo_in.valid_in.read();
		const bool ready = noc_plo_in.ready_out.read();
		if (valid && ready) {
			write_addr_fifo_pop_in.write(true);
			spm_req_fifo_push_in.write(true);
		} else {
			write_addr_fifo_pop_in.write(false);
			spm_req_fifo_push_in.write(false);
		}
	}

	// Combinational logic to drive the SPM request output based on the corresponding FIFO for PLO plane, and whether the plane is enabled, to allow pushing requests to SPM for bypassing logic
	void comb_spm_write_req_valid() {
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;

		spm_req_payload[RECV_PLANE].write(spm_req_fifo_data_out.read());
		spm_req_valid[RECV_PLANE].write(plane_on_plo && !spm_req_fifo_empty_out.read());
	}

	// Combinational logic to pop the SPM request FIFO for PLO plane when the SPM request handshakes, to allow pushing requests to SPM for bypassing logic
	void comb_spm_write_req_pop() {
		const uint32_t plane_en = plane_en_reg.read().to_uint();
		const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;
		const bool valid = !spm_req_fifo_empty_out.read();
		const bool ready = spm_req_ready[RECV_PLANE].read();

		if (valid && ready && plane_on_plo) {
			spm_req_fifo_pop_in.write(true);
		} else {
			spm_req_fifo_pop_in.write(false);
		}
	}

	// Sequential process to handle the main data flow and state updates of the HDDU
	void seq_process() {
		reset_internal();
		wait();

		while (true) {
			for (int i = 0; i < NUM_AGU; ++i) {
				agu_cfg_write_sig[i].write(false);
				agu_start_sig[i].write(false);
				agu_stop_sig[i].write(false);
			}
			for (int i = 0; i < NUM_SEND_PLANES; ++i) {
				noc_req_fifo_clear_sig[i].write(false);
				read_noc_addr_wait_fifo_clear_sig[i].write(false);
			}
			write_addr_fifo_clear_sig.write(false);
			spm_req_fifo_clear_sig.write(false);

			route_mmio_to_agu(mmio_addr.read(), mmio_wdata.read(), mmio_write.read());
			if (mmio_write.read()) {
				handle_global_mmio_write(mmio_addr.read(), mmio_wdata.read());
			}

			bool any_busy = false;
			for (int i = 0; i < NUM_AGU; ++i) {
				any_busy = any_busy || agu_busy_sig[i].read();
			}

			const bool global_ctrl_write =
				mmio_write.read() && (mmio_addr.read().to_uint() == MMIO_GLOBAL_CTRL);
			const sc_uint<32> ctrl_wdata = mmio_wdata.read();
			const bool global_reset_cmd = global_ctrl_write && ctrl_wdata[(int)HdduCtrllBit::CTRL_RESET];
			const bool global_start_cmd = global_ctrl_write && ctrl_wdata[(int)HdduCtrllBit::CTRL_START];
			const bool global_stop_cmd = global_ctrl_write && ctrl_wdata[(int)HdduCtrllBit::CTRL_STOP];

			if (global_reset_cmd) {
				run_active_latched = false;
				done_latched = false;
				prev_any_busy_latched = false;
			}
			if (global_start_cmd) {
				run_active_latched = true;
				done_latched = false;
			}
			if (global_stop_cmd) {
				run_active_latched = false;
				done_latched = false;
			}

			const bool done_pulse = run_active_latched && prev_any_busy_latched && !any_busy;
			if (done_pulse) {
				done_latched = true;
				run_active_latched = false;
			}
			prev_any_busy_latched = any_busy;

			sc_uint<32> status = global_status_reg.read();
			status[(int)HdduStatusBit::BUSY] = any_busy;
			status[(int)HdduStatusBit::DONE] = done_latched;
			// Clear start_pending_ once real AGU activity is reflected
			if (any_busy || done_latched) start_pending_ = false;
			status[(int)HdduStatusBit::ERROR] = (err_code_reg.read() != 0);
			status[(int)HdduStatusBit::IDLE] = !run_active_latched && !any_busy && !done_latched && (err_code_reg.read() == 0);

			const uint32_t plane_en = plane_en_reg.read().to_uint();
			const bool plane_on_ps = ((plane_en >> PLANE_PS) & 0x1u) != 0;
			const bool plane_on_pd = ((plane_en >> PLANE_PD) & 0x1u) != 0;
			const bool plane_on_pli = ((plane_en >> PLANE_PLI) & 0x1u) != 0;
			const bool plane_on_plo = ((plane_en >> PLANE_PLO) & 0x1u) != 0;

			unsigned stall_inc = 0;
			unsigned tx_pkt_inc = 0;
			unsigned tx_byte_inc = 0;
			unsigned rx_byte_inc = 0;

			// --- Counter Logic ---
			// Count TX Packets and Bytes (Sent to NoC)
			if (noc_ps_out.valid_out.read() && noc_ps_out.ready_in.read()) {
				tx_pkt_inc++;
				tx_byte_inc += DATA_BYTES;
			}
			if (noc_pd_out.valid_out.read() && noc_pd_out.ready_in.read()) {
				tx_pkt_inc++;
				tx_byte_inc += DATA_BYTES;
			}
			if (noc_pli_out.valid_out.read() && noc_pli_out.ready_in.read()) {
				tx_pkt_inc++;
				tx_byte_inc += DATA_BYTES;
			}
			// PLO Requests being sent also count as packets?
			if (noc_plo_out.valid_out.read() && noc_plo_out.ready_in.read()) {
				tx_pkt_inc++;
				// Addnoc_req_fifo_full_outrall, but maybe count as overhead?
			    // For now, let's just count data packets or requests.
			}

			// Count RX Bytes (Received from NoC at PLO)
			if (noc_plo_in.valid_in.read() && noc_plo_in.ready_out.read()) {
				rx_byte_inc += DATA_BYTES;
			}

			// Count Stalls (Any AGU valid but not ready)
			for (int i = 0; i < NUM_AGU; ++i) {
				if (agu_gen_valid_sig[i].read() && !agu_gen_ready_sig[i].read()) {
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
				status[(int)HdduStatusBit::STALL] = true;
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

			interrupt.write(status[(int)HdduStatusBit::ERROR] || status[(int)HdduStatusBit::DONE]);


			// --- Debugging messages ---
			// HDDU request issue sent to SPM for PS/PD/PLI planes
			for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
				if (spm_req_valid[lane].read() && spm_req_ready[lane].read()) {
					DEBUG_MSG("[HDDU][SPM] SPM_REQ_ISSUE"
							  << " lane=" << lane
							  << " addr=0x" << std::hex << spm_req_payload[lane].read().addr
							  << " wdata=0x" << spm_req_payload[lane].read().wdata
							  << " wen=" << spm_req_payload[lane].read().wen
							  << std::dec,
							  DEBUG_LEVEL_CLUSTER_COMPONENTS);
				}
			}

			// HDDU response received from SPM for PS/PD/PLI planes (read response)
			for (int lane = 0; lane < NUM_SEND_PLANES; ++lane) {
				if (spm_resp_valid[lane].read() && spm_resp_ready[lane].read()) {
					DEBUG_MSG("[HDDU][SPM] SPM_RESP_RECEIVE"
							  << " lane=" << lane
							  << " rdata=0x" << std::hex << spm_resp_payload[lane].read().rdata
							  << std::dec,
							  DEBUG_LEVEL_CLUSTER_COMPONENTS);
				}
			}

			// HDDU write response received from SPM for PLO plane (write confirmation)
			// SPM generates an immediate write response for every write request.
			// We consume it here and verify the response code to detect write errors.
			if (spm_resp_valid[PLANE_PLO].read() && spm_resp_ready[PLANE_PLO].read()) {
				const auto resp_code = spm_resp_payload[PLANE_PLO].read().code;
				const auto wr_addr   = spm_req_payload[PLANE_PLO].read().addr;
				const auto wr_data   = spm_req_payload[PLANE_PLO].read().wdata;
				DEBUG_MSG("[HDDU][SPM] SPM_WRITE_RESP_RECEIVE"
						  << " lane=" << PLANE_PLO
						  << " addr=0x" << std::hex << wr_addr
						  << " wdata=0x" << wr_data
						  << " code=" << static_cast<int>(resp_code)
						  << std::dec,
						  DEBUG_LEVEL_CLUSTER_COMPONENTS);
				if (resp_code != SPM_RESPONSE_CODE::SPM_OK) {
					DEBUG_MSG("[HDDU][SPM][ERROR] PLO write verification FAILED!"
							  << " lane=" << PLANE_PLO
							  << " addr=0x" << std::hex << wr_addr
							  << " expected SPM_OK but got code=" << static_cast<int>(resp_code)
							  << std::dec,
							  DEBUG_LEVEL_CLUSTER_COMPONENTS);
					err_code_reg.write(static_cast<uint32_t>(HdduErrorCode::SPM_ERROR));
					err_info0_reg.write(err_info0_reg.read() | (1u << PLANE_PLO));
					err_info1_reg.write(static_cast<uint32_t>(wr_addr));
				}
			}

			wait();
		}
	}
};

} // namespace cluster
} // namespace hybridacc
