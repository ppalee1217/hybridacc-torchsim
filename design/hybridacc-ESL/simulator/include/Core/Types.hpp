#pragma once

#include <systemc>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

static constexpr unsigned kCoreXlen = 32;
static constexpr unsigned kCoreGprCount = 32;

static constexpr uint32_t kIsramBase = 0x00000000u;
static constexpr uint32_t kDataSramBase = 0x10000000u;
static constexpr uint32_t kLocalMmioBase = 0x20000000u;
static constexpr uint32_t kLocalMmioEnd = 0x20000FFFu;
static constexpr uint32_t kDmaMmioBase = 0x20001000u;
static constexpr uint32_t kDmaMmioEnd = 0x20001FFFu;
static constexpr uint32_t kDmaStreamBase = 0x20001800u;
static constexpr uint32_t kDmaStreamEnd = 0x200018FFu;
static constexpr uint32_t kLocalTimerBase = 0x20002000u;
static constexpr uint32_t kLocalTimerEnd = 0x20002FFFu;
static constexpr uint32_t kPlicBase = 0x0C000000u;
static constexpr uint32_t kPlicEnd = 0x0C00FFFFu;
static constexpr uint32_t kClusterMmioBase = 0x40000000u;
static constexpr uint32_t kClusterMmioEnd = 0x400FFFFFu;
static constexpr uint32_t kClusterBroadcastBase = 0x50000000u;
static constexpr uint32_t kClusterBroadcastEnd = 0x5000FFFFu;
static constexpr uint32_t kNluMmioBase = 0x60000000u;
static constexpr uint32_t kNluMmioEnd = 0x6000FFFFu;

static constexpr uint32_t kClusterStride = 0x00010000u;
static constexpr uint32_t kNluStride = 0x00001000u;

static constexpr uint32_t kLocalCoreStatusOffset = 0x000u;
static constexpr uint32_t kLocalCoreCtrlOffset = 0x004u;
static constexpr uint32_t kLocalClusterMaskLoOffset = 0x008u;
static constexpr uint32_t kLocalClusterMaskHiOffset = 0x00Cu;
static constexpr uint32_t kLocalMmioErrStatusOffset = 0x010u;
static constexpr uint32_t kLocalLastTargetIdOffset = 0x014u;
static constexpr uint32_t kLocalLastFaultAddrOffset = 0x018u;
static constexpr uint32_t kLocalLastFaultInfoOffset = 0x01Cu;
static constexpr uint32_t kLocalDmaStatusOffset = 0x020u;
static constexpr uint32_t kLocalDmaErrCodeOffset = 0x024u;
static constexpr uint32_t kLocalTraceCtrlOffset = 0x028u;
static constexpr uint32_t kLocalTraceWptrOffset = 0x02Cu;
static constexpr uint32_t kLocalCycleCntLoOffset = 0x030u;
static constexpr uint32_t kLocalCycleCntHiOffset = 0x034u;
static constexpr uint32_t kLocalInstretLoOffset = 0x038u;
static constexpr uint32_t kLocalInstretHiOffset = 0x03Cu;
static constexpr uint32_t kLocalSwIrqSetOffset = 0x040u;
static constexpr uint32_t kLocalSwIrqClrOffset = 0x044u;
static constexpr uint32_t kLocalBootReasonOffset = 0x048u;

static constexpr uint32_t kCoreStatusRunningBit = 1u << 0;
static constexpr uint32_t kCoreStatusInTrapBit = 1u << 1;
static constexpr uint32_t kCoreStatusFaultBit = 1u << 2;
static constexpr uint32_t kCoreStatusHaltedBit = 1u << 3;

static constexpr uint32_t kCoreCtrlHaltReqBit = 1u << 0;
static constexpr uint32_t kCoreCtrlSingleStepBit = 1u << 1;
static constexpr uint32_t kCoreCtrlResumeBit = 1u << 2;

static constexpr uint32_t kDmaCtrlStartBit = 1u << 0;
static constexpr uint32_t kDmaStatusBusyBit = 1u << 0;
static constexpr uint32_t kDmaStatusDoneBit = 1u << 1;
static constexpr uint32_t kDmaStatusErrorBit = 1u << 2;

static constexpr uint32_t kPlicPendingLoOffset = 0x000u;
static constexpr uint32_t kPlicEnableLoOffset = 0x004u;
static constexpr uint32_t kPlicClaimOffset = 0x008u;
static constexpr uint32_t kPlicCompleteOffset = 0x00Cu;

enum class SectionType : uint8_t {
	CORE = 0,
	JOB = 1,
	BLOCK = 2,
	PROFILE = 3,
	DMA = 4,
	AGU = 5,
	NLU = 6,
	PE = 7,
	SCAN = 8,
	PATCH = 9,
	DEBUG = 10,
};

enum class DestinationKind : uint8_t {
	ISRAM = 0,
	DATA_SRAM = 1,
	EVENT_SRAM = 2,
};

struct ManifestHeader {
	SectionType section_type = SectionType::CORE;
	DestinationKind dst_kind = DestinationKind::ISRAM;
	uint32_t dram_base = 0;
	uint32_t dst_base = 0;
	uint32_t cluster_mask = 0;
	uint32_t word_count = 0;

	bool operator==(const ManifestHeader& other) const {
		return section_type == other.section_type && dst_kind == other.dst_kind && dram_base == other.dram_base &&
			dst_base == other.dst_base && cluster_mask == other.cluster_mask && word_count == other.word_count;
	}
};

struct ManifestPayloadBeat {
	uint32_t data = 0;
	bool last = false;

	bool operator==(const ManifestPayloadBeat& other) const {
		return data == other.data && last == other.last;
	}
};

struct ManifestPacket {
	SectionType section_type = SectionType::CORE;
	DestinationKind dst_kind = DestinationKind::ISRAM;
	uint32_t dram_base = 0;
	uint32_t dst_base = 0;
	uint32_t cluster_mask = 0;
	std::vector<uint32_t> payload_words;

	ManifestHeader header() const {
		return ManifestHeader{section_type, dst_kind, dram_base, dst_base, cluster_mask, static_cast<uint32_t>(payload_words.size())};
	}
};

struct MmioRequest {
	bool write = false;
	uint32_t addr = 0;
	uint32_t wdata = 0;
	uint8_t wstrb = 0;

	bool operator==(const MmioRequest& other) const {
		return write == other.write && addr == other.addr && wdata == other.wdata && wstrb == other.wstrb;
	}
};

struct MmioResponse {
	bool error = false;
	uint32_t rdata = 0;
	uint32_t error_code = 0;

	bool operator==(const MmioResponse& other) const {
		return error == other.error && rdata == other.rdata && error_code == other.error_code;
	}
};

struct ClusterMmioRequest {
	bool write = false;
	bool is_broadcast = false;
	uint32_t target_id = 0;
	uint32_t target_mask = 0;
	uint32_t addr = 0;
	uint32_t wdata = 0;
	uint8_t wstrb = 0;

	bool operator==(const ClusterMmioRequest& other) const {
		return write == other.write && is_broadcast == other.is_broadcast && target_id == other.target_id &&
			target_mask == other.target_mask && addr == other.addr && wdata == other.wdata && wstrb == other.wstrb;
	}
};

struct NluMmioRequest {
	bool write = false;
	uint32_t target_id = 0;
	uint32_t target_mask = 0;
	uint32_t addr = 0;
	uint32_t wdata = 0;
	uint8_t wstrb = 0;

	bool operator==(const NluMmioRequest& other) const {
		return write == other.write && target_id == other.target_id && target_mask == other.target_mask && addr == other.addr &&
			wdata == other.wdata && wstrb == other.wstrb;
	}
};

struct DmaMmioRequest {
	bool write = false;
	uint32_t addr = 0;
	uint32_t wdata = 0;
	uint8_t wstrb = 0;

	bool operator==(const DmaMmioRequest& other) const {
		return write == other.write && addr == other.addr && wdata == other.wdata && wstrb == other.wstrb;
	}
};

struct DmaRequest {
	bool write = true;
	uint32_t cluster_mask = 0;
	uint32_t addr = 0;
	uint64_t data = 0;
	uint32_t word_count = 0;

	bool operator==(const DmaRequest& other) const {
		return write == other.write && cluster_mask == other.cluster_mask && addr == other.addr && data == other.data &&
			word_count == other.word_count;
	}
};

inline std::ostream& operator<<(std::ostream& os, SectionType type) {
	return os << static_cast<unsigned>(type);
}

inline std::ostream& operator<<(std::ostream& os, DestinationKind kind) {
	return os << static_cast<unsigned>(kind);
}

inline std::ostream& operator<<(std::ostream& os, const ManifestHeader& header) {
	os << "ManifestHeader{section=" << header.section_type << ", dst=" << header.dst_kind << ", dst_base=0x" << std::hex
	   << header.dst_base << ", words=" << std::dec << header.word_count << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const ManifestPayloadBeat& beat) {
	os << "ManifestPayloadBeat{data=0x" << std::hex << beat.data << ", last=" << std::dec << beat.last << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const MmioRequest& request) {
	os << "MmioRequest{write=" << request.write << ", addr=0x" << std::hex << request.addr << ", data=0x" << request.wdata
	   << std::dec << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const MmioResponse& response) {
	os << "MmioResponse{error=" << response.error << ", rdata=0x" << std::hex << response.rdata << ", code=" << std::dec
	   << response.error_code << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const ClusterMmioRequest& request) {
	os << "ClusterMmioRequest{write=" << request.write << ", bcast=" << request.is_broadcast << ", id=" << request.target_id
	   << ", mask=0x" << std::hex << request.target_mask << ", addr=0x" << request.addr << ", data=0x" << request.wdata
	   << std::dec << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const NluMmioRequest& request) {
	os << "NluMmioRequest{id=" << request.target_id << ", mask=0x" << std::hex << request.target_mask << ", addr=0x"
	   << request.addr << ", data=0x" << request.wdata << std::dec << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const DmaMmioRequest& request) {
	os << "DmaMmioRequest{write=" << request.write << ", addr=0x" << std::hex << request.addr << ", data=0x"
	   << request.wdata << std::dec << "}";
	return os;
}

inline std::ostream& operator<<(std::ostream& os, const DmaRequest& request) {
	os << "DmaRequest{mask=0x" << std::hex << request.cluster_mask << ", addr=0x" << request.addr << ", data=0x"
	   << request.data << ", words=" << std::dec << request.word_count << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const SectionType& type, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(type), name);
}

inline void sc_trace(sc_trace_file* tf, const DestinationKind& kind, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(kind), name);
}

inline void sc_trace(sc_trace_file* tf, const ManifestHeader& header, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, header.section_type, name + ".section_type");
	sc_trace(tf, header.dst_kind, name + ".dst_kind");
	sc_trace(tf, header.dram_base, name + ".dram_base");
	sc_trace(tf, header.dst_base, name + ".dst_base");
	sc_trace(tf, header.cluster_mask, name + ".cluster_mask");
	sc_trace(tf, header.word_count, name + ".word_count");
}

inline void sc_trace(sc_trace_file* tf, const ManifestPayloadBeat& beat, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, beat.data, name + ".data");
	sc_trace(tf, beat.last, name + ".last");
}

inline void sc_trace(sc_trace_file* tf, const MmioRequest& request, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, request.write, name + ".write");
	sc_trace(tf, request.addr, name + ".addr");
	sc_trace(tf, request.wdata, name + ".wdata");
	sc_trace(tf, static_cast<uint32_t>(request.wstrb), name + ".wstrb");
}

inline void sc_trace(sc_trace_file* tf, const MmioResponse& response, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, response.error, name + ".error");
	sc_trace(tf, response.rdata, name + ".rdata");
	sc_trace(tf, response.error_code, name + ".error_code");
}

inline void sc_trace(sc_trace_file* tf, const ClusterMmioRequest& request, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, request.write, name + ".write");
	sc_trace(tf, request.is_broadcast, name + ".is_broadcast");
	sc_trace(tf, request.target_id, name + ".target_id");
	sc_trace(tf, request.target_mask, name + ".target_mask");
	sc_trace(tf, request.addr, name + ".addr");
	sc_trace(tf, request.wdata, name + ".wdata");
	sc_trace(tf, static_cast<uint32_t>(request.wstrb), name + ".wstrb");
}

inline void sc_trace(sc_trace_file* tf, const NluMmioRequest& request, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, request.write, name + ".write");
	sc_trace(tf, request.target_id, name + ".target_id");
	sc_trace(tf, request.target_mask, name + ".target_mask");
	sc_trace(tf, request.addr, name + ".addr");
	sc_trace(tf, request.wdata, name + ".wdata");
	sc_trace(tf, static_cast<uint32_t>(request.wstrb), name + ".wstrb");
}

inline void sc_trace(sc_trace_file* tf, const DmaMmioRequest& request, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, request.write, name + ".write");
	sc_trace(tf, request.addr, name + ".addr");
	sc_trace(tf, request.wdata, name + ".wdata");
	sc_trace(tf, static_cast<uint32_t>(request.wstrb), name + ".wstrb");
}

inline void sc_trace(sc_trace_file* tf, const DmaRequest& request, const std::string& name) {
	using sc_core::sc_trace;
	sc_trace(tf, request.write, name + ".write");
	sc_trace(tf, request.cluster_mask, name + ".cluster_mask");
	sc_trace(tf, request.addr, name + ".addr");
	sc_trace(tf, static_cast<uint32_t>(request.data & 0xFFFFFFFFu), name + ".data_lo");
	sc_trace(tf, static_cast<uint32_t>(request.data >> 32), name + ".data_hi");
	sc_trace(tf, request.word_count, name + ".word_count");
}

inline bool is_local_mmio(uint32_t addr) {
	return addr >= kLocalMmioBase && addr <= kLocalMmioEnd;
}

inline bool is_dma_mmio(uint32_t addr) {
	return addr >= kDmaMmioBase && addr <= kDmaMmioEnd;
}

inline bool is_dma_stream_window(uint32_t addr) {
	return addr >= kDmaStreamBase && addr <= kDmaStreamEnd;
}

inline bool is_plic_mmio(uint32_t addr) {
	return addr >= kPlicBase && addr <= kPlicEnd;
}

inline bool is_cluster_unicast_mmio(uint32_t addr) {
	return addr >= kClusterMmioBase && addr <= kClusterMmioEnd;
}

inline bool is_cluster_broadcast_mmio(uint32_t addr) {
	return addr >= kClusterBroadcastBase && addr <= kClusterBroadcastEnd;
}

inline bool is_nlu_mmio(uint32_t addr) {
	return addr >= kNluMmioBase && addr <= kNluMmioEnd;
}

inline bool is_data_sram_addr(uint32_t addr, uint32_t data_sram_bytes) {
	return addr >= kDataSramBase && addr < (kDataSramBase + data_sram_bytes);
}

} // namespace hybridacc::core
