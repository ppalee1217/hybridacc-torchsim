#pragma once

#include <systemc>

#include <array>
#include <cstdint>

#include "ComputeCluster.hpp"
#include "Core/Types.hpp"

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

template <unsigned NUM_CLUSTERS = 4>
SC_MODULE(ComputeClusterBusAdapter) {
public:
	sc_in<bool> clk{"clk"};
	sc_in<bool> reset_n{"reset_n"};

	sc_in<bool> dma_req_valid_i{"dma_req_valid_i"};
	sc_in<DmaRequest> dma_req_i{"dma_req_i"};
	sc_out<bool> dma_req_ready_o{"dma_req_ready_o"};
	sc_in<bool> cluster_cmd_valid_i{"cluster_cmd_valid_i"};
	sc_in<ClusterCommand> cluster_cmd_i{"cluster_cmd_i"};
	sc_out<bool> cluster_cmd_ready_o{"cluster_cmd_ready_o"};

	sc_vector<sc_signal<bool>> power_enable_sig_{"power_enable_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> interrupt_sig_{"interrupt_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_awvalid_sig_{"s_axi_awvalid_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_awready_sig_{"s_axi_awready_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<32>>> s_axi_awaddr_sig_{"s_axi_awaddr_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_wvalid_sig_{"s_axi_wvalid_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_wready_sig_{"s_axi_wready_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_biguint<64>>> s_axi_wdata_sig_{"s_axi_wdata_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<8>>> s_axi_wstrb_sig_{"s_axi_wstrb_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_bvalid_sig_{"s_axi_bvalid_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_bready_sig_{"s_axi_bready_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<2>>> s_axi_bresp_sig_{"s_axi_bresp_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_arvalid_sig_{"s_axi_arvalid_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_arready_sig_{"s_axi_arready_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<32>>> s_axi_araddr_sig_{"s_axi_araddr_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_rvalid_sig_{"s_axi_rvalid_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> s_axi_rready_sig_{"s_axi_rready_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_biguint<64>>> s_axi_rdata_sig_{"s_axi_rdata_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<2>>> s_axi_rresp_sig_{"s_axi_rresp_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> hsel_sig_{"hsel_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<32>>> haddr_sig_{"haddr_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> hwrite_sig_{"hwrite_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<2>>> htrans_sig_{"htrans_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<3>>> hsize_sig_{"hsize_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<3>>> hburst_sig_{"hburst_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<4>>> hprot_sig_{"hprot_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> hready_i_sig_{"hready_i_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<32>>> hwdata_sig_{"hwdata_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> hready_o_sig_{"hready_o_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<bool>> hresp_sig_{"hresp_sig", NUM_CLUSTERS};
	sc_vector<sc_signal<sc_uint<32>>> hrdata_sig_{"hrdata_sig", NUM_CLUSTERS};

	SC_CTOR(ComputeClusterBusAdapter) {
		SC_CTHREAD(seq_process, clk.pos());
		reset_signal_is(reset_n, false);
	}

	template <typename ClusterType>
	void bind_cluster(unsigned index, ClusterType& cluster) {
		cluster.clk(clk);
		cluster.reset_n(reset_n);
		cluster.power_enable_i(power_enable_sig_[index]);
		cluster.interrupt_o(interrupt_sig_[index]);
		cluster.s_axi_awvalid_i(s_axi_awvalid_sig_[index]);
		cluster.s_axi_awready_o(s_axi_awready_sig_[index]);
		cluster.s_axi_awaddr_i(s_axi_awaddr_sig_[index]);
		cluster.s_axi_wvalid_i(s_axi_wvalid_sig_[index]);
		cluster.s_axi_wready_o(s_axi_wready_sig_[index]);
		cluster.s_axi_wdata_i(s_axi_wdata_sig_[index]);
		cluster.s_axi_wstrb_i(s_axi_wstrb_sig_[index]);
		cluster.s_axi_bvalid_o(s_axi_bvalid_sig_[index]);
		cluster.s_axi_bready_i(s_axi_bready_sig_[index]);
		cluster.s_axi_bresp_o(s_axi_bresp_sig_[index]);
		cluster.s_axi_arvalid_i(s_axi_arvalid_sig_[index]);
		cluster.s_axi_arready_o(s_axi_arready_sig_[index]);
		cluster.s_axi_araddr_i(s_axi_araddr_sig_[index]);
		cluster.s_axi_rvalid_o(s_axi_rvalid_sig_[index]);
		cluster.s_axi_rready_i(s_axi_rready_sig_[index]);
		cluster.s_axi_rdata_o(s_axi_rdata_sig_[index]);
		cluster.s_axi_rresp_o(s_axi_rresp_sig_[index]);
		cluster.hsel_i(hsel_sig_[index]);
		cluster.haddr_i(haddr_sig_[index]);
		cluster.hwrite_i(hwrite_sig_[index]);
		cluster.htrans_i(htrans_sig_[index]);
		cluster.hsize_i(hsize_sig_[index]);
		cluster.hburst_i(hburst_sig_[index]);
		cluster.hprot_i(hprot_sig_[index]);
		cluster.hready_i(hready_i_sig_[index]);
		cluster.hwdata_i(hwdata_sig_[index]);
		cluster.hready_o(hready_o_sig_[index]);
		cluster.hresp_o(hresp_sig_[index]);
		cluster.hrdata_o(hrdata_sig_[index]);
	}

	uint32_t ahb_write_count() const { return ahb_write_count_reg_; }
	uint32_t dma_write_count() const { return dma_write_count_reg_; }
	uint32_t last_noc_cmd_data(unsigned index) const { return index < NUM_CLUSTERS ? last_noc_cmd_data_reg_[index] : 0u; }
	uint32_t last_axi_addr(unsigned index) const { return index < NUM_CLUSTERS ? last_axi_addr_reg_[index] : 0u; }
	uint64_t last_axi_data(unsigned index) const { return index < NUM_CLUSTERS ? last_axi_data_reg_[index] : 0ull; }

private:
	uint32_t ahb_write_count_reg_ = 0u;
	uint32_t dma_write_count_reg_ = 0u;
	std::array<uint32_t, NUM_CLUSTERS> last_noc_cmd_data_reg_{};
	std::array<uint32_t, NUM_CLUSTERS> last_axi_addr_reg_{};
	std::array<uint64_t, NUM_CLUSTERS> last_axi_data_reg_{};

	unsigned lowest_bit(uint32_t mask) const {
		for (unsigned index = 0; index < NUM_CLUSTERS; ++index) {
			if ((mask & (1u << index)) != 0u) {
				return index;
			}
		}
		return 0u;
	}

	void seq_process() {
		dma_req_ready_o.write(true);
		cluster_cmd_ready_o.write(true);
		for (unsigned index = 0; index < NUM_CLUSTERS; ++index) {
			power_enable_sig_[index].write(true);
			s_axi_awvalid_sig_[index].write(false);
			s_axi_awaddr_sig_[index].write(0u);
			s_axi_wvalid_sig_[index].write(false);
			s_axi_wdata_sig_[index].write(0u);
			s_axi_wstrb_sig_[index].write(0xFFu);
			s_axi_bready_sig_[index].write(true);
			s_axi_arvalid_sig_[index].write(false);
			s_axi_araddr_sig_[index].write(0u);
			s_axi_rready_sig_[index].write(true);
			hsel_sig_[index].write(false);
			haddr_sig_[index].write(0u);
			hwrite_sig_[index].write(false);
			htrans_sig_[index].write(0u);
			hsize_sig_[index].write(2u);
			hburst_sig_[index].write(0u);
			hprot_sig_[index].write(0u);
			hready_i_sig_[index].write(true);
			hwdata_sig_[index].write(0u);
		}
		wait();

		while (true) {
			for (unsigned index = 0; index < NUM_CLUSTERS; ++index) {
				s_axi_awvalid_sig_[index].write(false);
				s_axi_wvalid_sig_[index].write(false);
				hsel_sig_[index].write(false);
				htrans_sig_[index].write(0u);
			}

			if (dma_req_valid_i.read() && dma_req_ready_o.read()) {
				const DmaRequest req = dma_req_i.read();
				const unsigned target = lowest_bit(req.cluster_mask == 0u ? 1u : req.cluster_mask);
				s_axi_awvalid_sig_[target].write(true);
				s_axi_awaddr_sig_[target].write(req.addr);
				s_axi_wvalid_sig_[target].write(true);
				s_axi_wdata_sig_[target].write(req.data);
				s_axi_wstrb_sig_[target].write(0xFFu);
				last_axi_addr_reg_[target] = req.addr;
				last_axi_data_reg_[target] = req.data;
				dma_write_count_reg_ += 1u;
			}

			if (cluster_cmd_valid_i.read() && cluster_cmd_ready_o.read()) {
				const ClusterCommand cmd = cluster_cmd_i.read();
				const unsigned target = lowest_bit(cmd.cluster_mask == 0u ? 1u : cmd.cluster_mask);
				hsel_sig_[target].write(true);
				haddr_sig_[target].write(cmd.addr);
				hwrite_sig_[target].write(true);
				htrans_sig_[target].write(2u);
				hwdata_sig_[target].write(cmd.data);
				if (cmd.op == ClusterCommandOp::NOC_STREAM) {
					last_noc_cmd_data_reg_[target] = cmd.data;
				}
				ahb_write_count_reg_ += 1u;
			}

			wait();
		}
	}
};

} // namespace hybridacc::core