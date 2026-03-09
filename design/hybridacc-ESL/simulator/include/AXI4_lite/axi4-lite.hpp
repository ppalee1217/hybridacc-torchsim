#pragma once

#include <systemc>
#include <cstdint>

namespace hybridacc {
namespace axi4lite {

constexpr uint32_t AXI_RESP_OKAY = 0x0;
constexpr uint32_t AXI_RESP_EXOKAY = 0x1;
constexpr uint32_t AXI_RESP_SLVERR = 0x2;
constexpr uint32_t AXI_RESP_DECERR = 0x3;

constexpr unsigned AXI4L_DEFAULT_ADDR_WIDTH = 32;
constexpr unsigned AXI4L_DEFAULT_DATA_WIDTH = 64;
constexpr unsigned AXI4L_DEFAULT_STRB_WIDTH = AXI4L_DEFAULT_DATA_WIDTH / 8;

constexpr uint32_t AXI4L_REQ_QUEUE_DEPTH = 16;
constexpr uint32_t AXI4L_RESP_QUEUE_DEPTH = 16;

template <unsigned ADDR_WIDTH = AXI4L_DEFAULT_ADDR_WIDTH>
struct AxiLiteWriteAddr {
	sc_dt::sc_uint<ADDR_WIDTH> addr{0};
};

template <unsigned DATA_WIDTH = AXI4L_DEFAULT_DATA_WIDTH>
struct AxiLiteWriteData {
	sc_dt::sc_biguint<DATA_WIDTH> data{0};
	sc_dt::sc_uint<DATA_WIDTH / 8> strb{0};
};

struct AxiLiteWriteResp {
	sc_dt::sc_uint<2> resp{AXI_RESP_OKAY};
};

template <unsigned ADDR_WIDTH = AXI4L_DEFAULT_ADDR_WIDTH>
struct AxiLiteReadAddr {
	sc_dt::sc_uint<ADDR_WIDTH> addr{0};
};

template <unsigned DATA_WIDTH = AXI4L_DEFAULT_DATA_WIDTH>
struct AxiLiteReadData {
	sc_dt::sc_biguint<DATA_WIDTH> data{0};
	sc_dt::sc_uint<2> resp{AXI_RESP_OKAY};
};

} // namespace axi4lite
} // namespace hybridacc
