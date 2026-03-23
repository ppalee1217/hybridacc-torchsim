#pragma once

#include <systemc>

#include <array>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

using namespace sc_core;
using namespace sc_dt;

namespace hybridacc::core {

static constexpr unsigned kCoreXlen = 32;
static constexpr unsigned kCoreGprCount = 16;

static constexpr uint32_t kIsramBase = 0x00000000u;
static constexpr uint32_t kDataSramBase = 0x10000000u;
static constexpr uint32_t kLocalCsrBase = 0x20000000u;
static constexpr uint32_t kDmaMmioBase = 0x30000000u;
static constexpr uint32_t kClusterCmdBase = 0x40000000u;
static constexpr uint32_t kNluMmioBase = 0x50000000u;

static constexpr uint32_t kClusterStride = 0x00010000u;
static constexpr uint32_t kNluStride = 0x00001000u;

static constexpr uint32_t kCapNocCommandPath = 1u << 0;
static constexpr uint32_t kCapDmaStream = 1u << 1;
static constexpr uint32_t kCapNluPath = 1u << 2;
static constexpr uint32_t kCapBroadcastWrite = 1u << 3;
static constexpr uint32_t kCapIrqRouter = 1u << 4;
static constexpr uint32_t kCapSubroutine = 1u << 5;
static constexpr uint32_t kCapBlockExpander = 1u << 6;
static constexpr uint32_t kCapNonLinearUnit = kCapNluPath;

static constexpr uint32_t kCtrlStartBit = 1u << 0;
static constexpr uint32_t kCtrlAbortBit = 1u << 1;

static constexpr uint32_t kStatusBusyBit = 1u << 0;
static constexpr uint32_t kStatusDoneBit = 1u << 1;
static constexpr uint32_t kStatusErrorBit = 1u << 2;

static constexpr uint32_t kWaitEventDone = 1u << 0;
static constexpr uint32_t kWaitEventError = 1u << 1;

static constexpr uint32_t kCsrHaccCtrl = 0x080u >> 2;
static constexpr uint32_t kCsrHaccStatus = 0x084u >> 2;
static constexpr uint32_t kCsrHaccJobBaseLo = 0x088u >> 2;
static constexpr uint32_t kCsrHaccRangeBegin = 0x08Cu >> 2;
static constexpr uint32_t kCsrHaccRangeCount = 0x090u >> 2;
static constexpr uint32_t kCsrHaccErrCode = 0x094u >> 2;
static constexpr uint32_t kCsrHaccLastBlockId = 0x098u >> 2;
static constexpr uint32_t kCsrHaccPerfDmaCnt = 0x09Cu >> 2;
static constexpr uint32_t kCsrHaccPerfMmioCnt = 0x0A0u >> 2;

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

inline std::ostream& operator<<(std::ostream& os, SectionType type) {
	switch (type) {
	case SectionType::CORE: return os << "CORE";
	case SectionType::JOB: return os << "JOB";
	case SectionType::BLOCK: return os << "BLOCK";
	case SectionType::PROFILE: return os << "PROFILE";
	case SectionType::DMA: return os << "DMA";
	case SectionType::AGU: return os << "AGU";
	case SectionType::NLU: return os << "NLU";
	case SectionType::PE: return os << "PE";
	case SectionType::SCAN: return os << "SCAN";
	case SectionType::PATCH: return os << "PATCH";
	case SectionType::DEBUG: return os << "DEBUG";
	default: return os << "UNKNOWN_SECTION";
	}
}

inline void sc_trace(sc_trace_file* tf, const SectionType& type, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(type), name);
}

enum class DestinationKind : uint8_t {
	ISRAM = 0,
	DESC_SRAM = 1,
	EVENT_SRAM = 2,
	CLUSTER_DATA = 3,
};

inline std::ostream& operator<<(std::ostream& os, DestinationKind kind) {
	switch (kind) {
	case DestinationKind::ISRAM: return os << "ISRAM";
	case DestinationKind::DESC_SRAM: return os << "DESC_SRAM";
	case DestinationKind::EVENT_SRAM: return os << "EVENT_SRAM";
	case DestinationKind::CLUSTER_DATA: return os << "CLUSTER_DATA";
	default: return os << "UNKNOWN_DST";
	}
}

inline void sc_trace(sc_trace_file* tf, const DestinationKind& kind, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(kind), name);
}

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

inline std::ostream& operator<<(std::ostream& os, const ManifestHeader& header) {
	os << "ManifestHeader{sec=" << header.section_type << ", dst=" << header.dst_kind
	   << ", dst_base=0x" << std::hex << header.dst_base << ", words=" << std::dec << header.word_count << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const ManifestHeader& header, const std::string& name) {
	sc_core::sc_trace(tf, header.section_type, name + ".section_type");
	sc_core::sc_trace(tf, header.dst_kind, name + ".dst_kind");
	sc_core::sc_trace(tf, header.dram_base, name + ".dram_base");
	sc_core::sc_trace(tf, header.dst_base, name + ".dst_base");
	sc_core::sc_trace(tf, header.cluster_mask, name + ".cluster_mask");
	sc_core::sc_trace(tf, header.word_count, name + ".word_count");
}

struct ManifestPayloadBeat {
	uint32_t data = 0;
	bool last = false;

	bool operator==(const ManifestPayloadBeat& other) const {
		return data == other.data && last == other.last;
	}
};

inline std::ostream& operator<<(std::ostream& os, const ManifestPayloadBeat& beat) {
	os << "ManifestPayloadBeat{data=0x" << std::hex << beat.data << std::dec << ", last=" << beat.last << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const ManifestPayloadBeat& beat, const std::string& name) {
	sc_core::sc_trace(tf, beat.data, name + ".data");
	sc_core::sc_trace(tf, beat.last, name + ".last");
}

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

enum class ClusterCommandOp : uint8_t {
	NONE = 0,
	MMIO_WRITE = 1,
	NOC_STREAM = 2,
	HDDU_STREAM = 3,
	START = 4,
};

inline std::ostream& operator<<(std::ostream& os, ClusterCommandOp op) {
	switch (op) {
	case ClusterCommandOp::NONE: return os << "NONE";
	case ClusterCommandOp::MMIO_WRITE: return os << "MMIO_WRITE";
	case ClusterCommandOp::NOC_STREAM: return os << "NOC_STREAM";
	case ClusterCommandOp::HDDU_STREAM: return os << "HDDU_STREAM";
	case ClusterCommandOp::START: return os << "START";
	default: return os << "UNKNOWN_CLUSTER_OP";
	}
}

inline void sc_trace(sc_trace_file* tf, const ClusterCommandOp& op, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(op), name);
}

struct ClusterCommand {
	ClusterCommandOp op = ClusterCommandOp::NONE;
	uint32_t cluster_mask = 0;
	uint32_t addr = 0;
	uint32_t data = 0;
	uint8_t stream_flags = 0;

	bool operator==(const ClusterCommand& other) const {
		return op == other.op && cluster_mask == other.cluster_mask && addr == other.addr && data == other.data && stream_flags == other.stream_flags;
	}
};

inline std::ostream& operator<<(std::ostream& os, const ClusterCommand& cmd) {
	os << "ClusterCommand{op=" << cmd.op << ", mask=0x" << std::hex << cmd.cluster_mask
	   << ", addr=0x" << cmd.addr << ", data=0x" << cmd.data << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const ClusterCommand& cmd, const std::string& name) {
	sc_core::sc_trace(tf, cmd.op, name + ".op");
	sc_core::sc_trace(tf, cmd.cluster_mask, name + ".cluster_mask");
	sc_core::sc_trace(tf, cmd.addr, name + ".addr");
	sc_core::sc_trace(tf, cmd.data, name + ".data");
	sc_core::sc_trace(tf, static_cast<uint32_t>(cmd.stream_flags), name + ".stream_flags");
}

struct NluCommand {
	uint32_t nlu_mask = 0;
	uint32_t addr = 0;
	uint32_t data = 0;
	uint8_t stream_flags = 0;

	bool operator==(const NluCommand& other) const {
		return nlu_mask == other.nlu_mask && addr == other.addr && data == other.data && stream_flags == other.stream_flags;
	}
};

inline std::ostream& operator<<(std::ostream& os, const NluCommand& cmd) {
	os << "NluCommand{mask=0x" << std::hex << cmd.nlu_mask << ", addr=0x" << cmd.addr << ", data=0x" << cmd.data << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const NluCommand& cmd, const std::string& name) {
	sc_core::sc_trace(tf, cmd.nlu_mask, name + ".nlu_mask");
	sc_core::sc_trace(tf, cmd.addr, name + ".addr");
	sc_core::sc_trace(tf, cmd.data, name + ".data");
	sc_core::sc_trace(tf, static_cast<uint32_t>(cmd.stream_flags), name + ".stream_flags");
}

enum class DmaReqKind : uint8_t {
	NONE = 0,
	WRITE64 = 1,
	READ64 = 2,
};

inline std::ostream& operator<<(std::ostream& os, DmaReqKind kind) {
	switch (kind) {
	case DmaReqKind::NONE: return os << "NONE";
	case DmaReqKind::WRITE64: return os << "WRITE64";
	case DmaReqKind::READ64: return os << "READ64";
	default: return os << "UNKNOWN_DMA_KIND";
	}
}

inline void sc_trace(sc_trace_file* tf, const DmaReqKind& kind, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(kind), name);
}

struct DmaRequest {
	DmaReqKind kind = DmaReqKind::NONE;
	uint32_t cluster_mask = 0;
	uint32_t addr = 0;
	uint64_t data = 0;
	uint32_t word_count = 0;

	bool operator==(const DmaRequest& other) const {
		return kind == other.kind && cluster_mask == other.cluster_mask && addr == other.addr && data == other.data && word_count == other.word_count;
	}
};

inline std::ostream& operator<<(std::ostream& os, const DmaRequest& req) {
	os << "DmaRequest{kind=" << req.kind << ", mask=0x" << std::hex << req.cluster_mask
	   << ", addr=0x" << req.addr << ", data=0x" << req.data << std::dec << ", words=" << req.word_count << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const DmaRequest& req, const std::string& name) {
	sc_core::sc_trace(tf, req.kind, name + ".kind");
	sc_core::sc_trace(tf, req.cluster_mask, name + ".cluster_mask");
	sc_core::sc_trace(tf, req.addr, name + ".addr");
	sc_core::sc_trace(tf, static_cast<uint32_t>(req.data & 0xFFFFFFFFull), name + ".data_lo");
	sc_core::sc_trace(tf, static_cast<uint32_t>((req.data >> 32) & 0xFFFFFFFFull), name + ".data_hi");
	sc_core::sc_trace(tf, req.word_count, name + ".word_count");
}

struct DmaMmioReq {
	bool write = false;
	uint32_t addr = 0;
	uint32_t data = 0;

	bool operator==(const DmaMmioReq& other) const {
		return write == other.write && addr == other.addr && data == other.data;
	}
};

inline std::ostream& operator<<(std::ostream& os, const DmaMmioReq& req) {
	os << "DmaMmioReq{write=" << req.write << ", addr=0x" << std::hex << req.addr << ", data=0x" << req.data << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const DmaMmioReq& req, const std::string& name) {
	sc_core::sc_trace(tf, req.write, name + ".write");
	sc_core::sc_trace(tf, req.addr, name + ".addr");
	sc_core::sc_trace(tf, req.data, name + ".data");
}

struct DataFabricReq {
	bool valid = false;
	bool write = false;
	uint32_t cluster_mask = 0;
	uint32_t addr = 0;
	uint64_t data = 0;

	bool operator==(const DataFabricReq& other) const {
		return valid == other.valid && write == other.write && cluster_mask == other.cluster_mask && addr == other.addr && data == other.data;
	}
};

inline std::ostream& operator<<(std::ostream& os, const DataFabricReq& req) {
	os << "DataFabricReq{valid=" << req.valid << ", write=" << req.write << ", mask=0x" << std::hex << req.cluster_mask
	   << ", addr=0x" << req.addr << ", data=0x" << req.data << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const DataFabricReq& req, const std::string& name) {
	sc_core::sc_trace(tf, req.valid, name + ".valid");
	sc_core::sc_trace(tf, req.write, name + ".write");
	sc_core::sc_trace(tf, req.cluster_mask, name + ".cluster_mask");
	sc_core::sc_trace(tf, req.addr, name + ".addr");
	sc_core::sc_trace(tf, static_cast<uint32_t>(req.data & 0xFFFFFFFFull), name + ".data_lo");
	sc_core::sc_trace(tf, static_cast<uint32_t>((req.data >> 32) & 0xFFFFFFFFull), name + ".data_hi");
}

static constexpr uint32_t kSrZBit = 0u;
static constexpr uint32_t kSrNBit = 1u;
static constexpr uint32_t kSrCBit = 2u;
static constexpr uint32_t kSrVBit = 3u;
static constexpr uint32_t kSrIrqEnableBit = 4u;
static constexpr uint32_t kSrInIsrBit = 5u;
static constexpr uint32_t kSrFaultBit = 6u;

static constexpr uint32_t kCoreCtrlRunBit = 1u << 0;
static constexpr uint32_t kCoreCtrlHaltReqBit = 1u << 1;
static constexpr uint32_t kCoreCtrlSingleStepBit = 1u << 2;
static constexpr uint32_t kCoreCtrlClrFaultBit = 1u << 3;

static constexpr uint32_t kCoreStatusRunningBit = 1u << 0;
static constexpr uint32_t kCoreStatusHaltedBit = 1u << 1;
static constexpr uint32_t kCoreStatusFaultBit = 1u << 2;
static constexpr uint32_t kCoreStatusInIsrBit = 1u << 3;

static constexpr uint32_t kCsrCoreCtrl = 0x000u >> 2;
static constexpr uint32_t kCsrCoreStatus = 0x004u >> 2;
static constexpr uint32_t kCsrIrqPendingLo = 0x008u >> 2;
static constexpr uint32_t kCsrIrqPendingHi = 0x00Cu >> 2;
static constexpr uint32_t kCsrIrqEnableLo = 0x010u >> 2;
static constexpr uint32_t kCsrIrqEnableHi = 0x014u >> 2;
static constexpr uint32_t kCsrIrqAckLo = 0x018u >> 2;
static constexpr uint32_t kCsrIrqAckHi = 0x01Cu >> 2;
static constexpr uint32_t kCsrIrqCauseId = 0x020u >> 2;
static constexpr uint32_t kCsrEpc = 0x024u >> 2;
static constexpr uint32_t kCsrEsr = 0x028u >> 2;
static constexpr uint32_t kCsrFaultCode = 0x02Cu >> 2;
static constexpr uint32_t kCsrFaultAux = 0x030u >> 2;
static constexpr uint32_t kCsrCycleCntLo = 0x034u >> 2;
static constexpr uint32_t kCsrCycleCntHi = 0x038u >> 2;
static constexpr uint32_t kCsrInstretCntLo = 0x03Cu >> 2;
static constexpr uint32_t kCsrInstretCntHi = 0x040u >> 2;

enum class ErrorCode : uint8_t {
	NONE = 0,
	ILLEGAL_OPCODE = 1,
	UNALIGNED_WORD_ACCESS = 2,
	MMIO_PROTECTION_FAULT = 3,
	BROADCAST_TARGET_FAULT = 4,
	LOCAL_MEMORY_BOUNDS_FAULT = 5,
	COMMAND_ERROR = 6,
	IRET_OUTSIDE_ISR = 7,
	CAPABILITY_MISMATCH = 8,
};

inline std::ostream& operator<<(std::ostream& os, ErrorCode code) {
	switch (code) {
	case ErrorCode::NONE: return os << "NONE";
	case ErrorCode::ILLEGAL_OPCODE: return os << "ILLEGAL_OPCODE";
	case ErrorCode::UNALIGNED_WORD_ACCESS: return os << "UNALIGNED_WORD_ACCESS";
	case ErrorCode::MMIO_PROTECTION_FAULT: return os << "MMIO_PROTECTION_FAULT";
	case ErrorCode::BROADCAST_TARGET_FAULT: return os << "BROADCAST_TARGET_FAULT";
	case ErrorCode::LOCAL_MEMORY_BOUNDS_FAULT: return os << "LOCAL_MEMORY_BOUNDS_FAULT";
	case ErrorCode::COMMAND_ERROR: return os << "COMMAND_ERROR";
	case ErrorCode::IRET_OUTSIDE_ISR: return os << "IRET_OUTSIDE_ISR";
	case ErrorCode::CAPABILITY_MISMATCH: return os << "CAPABILITY_MISMATCH";
	default: return os << "UNKNOWN_ERROR";
	}
}

inline void sc_trace(sc_trace_file* tf, const ErrorCode& code, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(code), name);
}

enum class CommandKind : uint8_t {
	NONE = 0,
	MMIO_READ = 1,
	MMIO_WRITE = 2,
	MMIO_WRITE_BROADCAST = 3,
	MMIO_READ_BROADCAST = 4,
	STREAM_WORD = 5,
	STREAM_CTRL = 6,
};

inline std::ostream& operator<<(std::ostream& os, CommandKind kind) {
	switch (kind) {
	case CommandKind::NONE: return os << "NONE";
	case CommandKind::MMIO_READ: return os << "MMIO_READ";
	case CommandKind::MMIO_WRITE: return os << "MMIO_WRITE";
	case CommandKind::MMIO_WRITE_BROADCAST: return os << "MMIO_WRITE_BROADCAST";
	case CommandKind::MMIO_READ_BROADCAST: return os << "MMIO_READ_BROADCAST";
	case CommandKind::STREAM_WORD: return os << "STREAM_WORD";
	case CommandKind::STREAM_CTRL: return os << "STREAM_CTRL";
	default: return os << "UNKNOWN_COMMAND_KIND";
	}
}

inline void sc_trace(sc_trace_file* tf, const CommandKind& kind, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(kind), name);
}

enum class StreamDestination : uint8_t {
	DMA = 0,
	CLUSTER_NOC = 1,
	CLUSTER_HDDU = 2,
	NLU_CFG = 3,
};

inline std::ostream& operator<<(std::ostream& os, StreamDestination dst) {
	switch (dst) {
	case StreamDestination::DMA: return os << "DMA";
	case StreamDestination::CLUSTER_NOC: return os << "CLUSTER_NOC";
	case StreamDestination::CLUSTER_HDDU: return os << "CLUSTER_HDDU";
	case StreamDestination::NLU_CFG: return os << "NLU_CFG";
	default: return os << "UNKNOWN_STREAM_DST";
	}
}

inline void sc_trace(sc_trace_file* tf, const StreamDestination& dst, const std::string& name) {
	sc_core::sc_trace(tf, static_cast<uint32_t>(dst), name);
}

struct HaccJobDesc {
	uint32_t version = 0;
	uint32_t flags = 0;
	uint32_t block_desc_base = 0;
	uint32_t block_desc_count = 0;
	uint32_t profile_table_base = 0;
	uint32_t profile_table_count = 0;
	uint32_t dma_rule_base = 0;
	uint32_t dma_rule_count = 0;
	uint32_t agu_rule_base = 0;
	uint32_t agu_rule_count = 0;
	uint32_t patch_base = 0;
	uint32_t patch_count = 0;
	uint32_t pe_program_base = 0;
	uint32_t scan_chain_base = 0;
	uint32_t required_cluster_mask = 0;
	uint32_t required_caps = 0;

	bool operator==(const HaccJobDesc& other) const {
		return version == other.version && flags == other.flags && block_desc_base == other.block_desc_base &&
			block_desc_count == other.block_desc_count && profile_table_base == other.profile_table_base &&
			profile_table_count == other.profile_table_count && dma_rule_base == other.dma_rule_base &&
			dma_rule_count == other.dma_rule_count && agu_rule_base == other.agu_rule_base &&
			agu_rule_count == other.agu_rule_count && patch_base == other.patch_base &&
			patch_count == other.patch_count && pe_program_base == other.pe_program_base &&
			scan_chain_base == other.scan_chain_base && required_cluster_mask == other.required_cluster_mask &&
			required_caps == other.required_caps;
	}
};

inline std::ostream& operator<<(std::ostream& os, const HaccJobDesc& job) {
	os << "HaccJobDesc{ver=" << job.version << ", blocks=" << job.block_desc_count
	   << ", cluster_mask=0x" << std::hex << job.required_cluster_mask
	   << ", caps=0x" << job.required_caps << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const HaccJobDesc& job, const std::string& name) {
	sc_core::sc_trace(tf, job.version, name + ".version");
	sc_core::sc_trace(tf, job.flags, name + ".flags");
	sc_core::sc_trace(tf, job.block_desc_base, name + ".block_desc_base");
	sc_core::sc_trace(tf, job.block_desc_count, name + ".block_desc_count");
	sc_core::sc_trace(tf, job.required_cluster_mask, name + ".required_cluster_mask");
	sc_core::sc_trace(tf, job.required_caps, name + ".required_caps");
}

struct HaccBlockDesc {
	uint32_t block_id = 0;
	uint16_t profile_id = 0;
	uint16_t pe_program_id = 0;
	uint16_t loop_rank = 0;
	uint16_t flags = 0;
	uint32_t repeat_count = 0;
	uint32_t cluster_mask = 0;
	uint32_t section_map_seed = 0;

	bool operator==(const HaccBlockDesc& other) const {
		return block_id == other.block_id && profile_id == other.profile_id && pe_program_id == other.pe_program_id &&
			loop_rank == other.loop_rank && flags == other.flags && repeat_count == other.repeat_count &&
			cluster_mask == other.cluster_mask && section_map_seed == other.section_map_seed;
	}
};

inline std::ostream& operator<<(std::ostream& os, const HaccBlockDesc& block) {
	os << "HaccBlockDesc{id=" << block.block_id << ", profile=" << block.profile_id
	   << ", pe=" << block.pe_program_id << ", repeat=" << block.repeat_count
	   << ", cluster_mask=0x" << std::hex << block.cluster_mask << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const HaccBlockDesc& block, const std::string& name) {
	sc_core::sc_trace(tf, block.block_id, name + ".block_id");
	sc_core::sc_trace(tf, block.profile_id, name + ".profile_id");
	sc_core::sc_trace(tf, block.pe_program_id, name + ".pe_program_id");
	sc_core::sc_trace(tf, block.loop_rank, name + ".loop_rank");
	sc_core::sc_trace(tf, block.flags, name + ".flags");
	sc_core::sc_trace(tf, block.repeat_count, name + ".repeat_count");
	sc_core::sc_trace(tf, block.cluster_mask, name + ".cluster_mask");
	sc_core::sc_trace(tf, block.section_map_seed, name + ".section_map_seed");
}

struct McuCmdReq {
	CommandKind kind = CommandKind::NONE;
	StreamDestination stream_dst = StreamDestination::DMA;
	uint8_t stream_flags = 0;
	uint32_t target_mask = 0;
	uint32_t addr = 0;
	uint32_t data = 0;
	uint32_t word_count = 0;

	bool operator==(const McuCmdReq& other) const {
		return kind == other.kind && stream_dst == other.stream_dst && stream_flags == other.stream_flags &&
			target_mask == other.target_mask && addr == other.addr && data == other.data && word_count == other.word_count;
	}
};

inline std::ostream& operator<<(std::ostream& os, const McuCmdReq& req) {
	os << "McuCmdReq{kind=" << req.kind << ", dst=" << req.stream_dst
	   << ", flags=0x" << std::hex << static_cast<uint32_t>(req.stream_flags)
	   << ", mask=0x" << req.target_mask << ", addr=0x" << req.addr
	   << ", data=0x" << req.data << ", words=" << std::dec << req.word_count << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const McuCmdReq& req, const std::string& name) {
	sc_core::sc_trace(tf, req.kind, name + ".kind");
	sc_core::sc_trace(tf, req.stream_dst, name + ".stream_dst");
	sc_core::sc_trace(tf, static_cast<uint32_t>(req.stream_flags), name + ".stream_flags");
	sc_core::sc_trace(tf, req.target_mask, name + ".target_mask");
	sc_core::sc_trace(tf, req.addr, name + ".addr");
	sc_core::sc_trace(tf, req.data, name + ".data");
	sc_core::sc_trace(tf, req.word_count, name + ".word_count");
}

struct McuCmdResp {
	bool done = false;
	bool error = false;
	uint8_t error_target_id = 0;
	uint32_t rdata = 0;
	uint32_t aux = 0;

	bool operator==(const McuCmdResp& other) const {
		return done == other.done && error == other.error && error_target_id == other.error_target_id &&
			rdata == other.rdata && aux == other.aux;
	}
};

inline std::ostream& operator<<(std::ostream& os, const McuCmdResp& resp) {
	os << "McuCmdResp{done=" << resp.done << ", error=" << resp.error
	   << ", target=" << static_cast<uint32_t>(resp.error_target_id)
	   << ", rdata=0x" << std::hex << resp.rdata << ", aux=0x" << resp.aux << std::dec << "}";
	return os;
}

inline void sc_trace(sc_trace_file* tf, const McuCmdResp& resp, const std::string& name) {
	sc_core::sc_trace(tf, resp.done, name + ".done");
	sc_core::sc_trace(tf, resp.error, name + ".error");
	sc_core::sc_trace(tf, static_cast<uint32_t>(resp.error_target_id), name + ".error_target_id");
	sc_core::sc_trace(tf, resp.rdata, name + ".rdata");
	sc_core::sc_trace(tf, resp.aux, name + ".aux");
}

// Instruction encoding in this simulator is intentionally regularized for the
// SystemC model. Architectural semantics follow ISA.md, while helper encoders
// map those semantics onto a compact 32-bit simulator-local layout.
enum class Opcode : uint8_t {
	NOP = 0x00,
	MOVI = 0x01,
	MOVHI = 0x02,
	MOV = 0x03,
	ADD = 0x04,
	ADDI = 0x05,
	SUB = 0x06,
	AND = 0x07,
	OR = 0x08,
	XOR = 0x09,
	SHL = 0x0A,
	SHR = 0x0B,
	CMP = 0x0C,
	CMPI = 0x0D,
	B = 0x0E,
	BEQ = 0x0F,
	BNE = 0x10,
	BLT = 0x11,
	BGE = 0x12,
	CALL = 0x13,
	CALLR = 0x14,
	RET = 0x15,
	HLT = 0x16,
	LDW = 0x17,
	STW = 0x18,
	LDB = 0x19,
	STB = 0x1A,
	PUSH = 0x1B,
	POP = 0x1C,
	CSRRD = 0x1D,
	CSRWR = 0x1E,
	CSRSI = 0x1F,
	CSRCL = 0x20,
	MMIOW = 0x21,
	MMIOR = 0x22,
	MMIOWB = 0x23,
	MMIORD = 0x24,
	STRM = 0x25,
	STRMI = 0x26,
	STRMC = 0x27,
	WFI = 0x28,
	WAIT = 0x29,
	ACKIRQ = 0x2A,
	EI = 0x2B,
	DI = 0x2C,
	IRET = 0x2D,
};

inline uint8_t opcode_of(uint32_t instruction) {
	return static_cast<uint8_t>((instruction >> 26) & 0x3Fu);
}

inline uint8_t rd_of(uint32_t instruction) {
	return static_cast<uint8_t>((instruction >> 22) & 0x0Fu);
}

inline uint8_t rs1_of(uint32_t instruction) {
	return static_cast<uint8_t>((instruction >> 18) & 0x0Fu);
}

inline uint8_t rs2_of(uint32_t instruction) {
	return static_cast<uint8_t>((instruction >> 14) & 0x0Fu);
}

inline uint32_t func14_of(uint32_t instruction) {
	return instruction & 0x3FFFu;
}

inline int32_t sign_extend18(uint32_t value) {
	const uint32_t masked = value & 0x3FFFFu;
	return (masked & 0x20000u) != 0u ? static_cast<int32_t>(masked | 0xFFFC0000u) : static_cast<int32_t>(masked);
}

inline int32_t sign_extend26(uint32_t value) {
	const uint32_t masked = value & 0x03FFFFFFu;
	return (masked & 0x02000000u) != 0u ? static_cast<int32_t>(masked | 0xFC000000u) : static_cast<int32_t>(masked);
}

inline int32_t imm18_of(uint32_t instruction) {
	return sign_extend18(instruction & 0x3FFFFu);
}

inline int32_t imm26_of(uint32_t instruction) {
	return sign_extend26(instruction & 0x03FFFFFFu);
}

inline int32_t branch_offset_bytes18(uint32_t instruction) {
	return imm18_of(instruction) << 2;
}

inline int32_t branch_offset_bytes26(uint32_t instruction) {
	return imm26_of(instruction) << 2;
}

constexpr uint32_t mask_imm18(int32_t value) {
	return static_cast<uint32_t>(value) & 0x3FFFFu;
}

constexpr uint32_t mask_imm26(int32_t value) {
	return static_cast<uint32_t>(value) & 0x03FFFFFFu;
}

constexpr uint32_t encode_r(Opcode opcode, uint8_t rd, uint8_t rs1, uint8_t rs2, uint16_t func14 = 0u) {
	return (static_cast<uint32_t>(opcode) << 26) |
		((static_cast<uint32_t>(rd) & 0x0Fu) << 22) |
		((static_cast<uint32_t>(rs1) & 0x0Fu) << 18) |
		((static_cast<uint32_t>(rs2) & 0x0Fu) << 14) |
		(static_cast<uint32_t>(func14) & 0x3FFFu);
}

constexpr uint32_t encode_i(Opcode opcode, uint8_t rd, uint8_t rs1, int32_t imm18) {
	return (static_cast<uint32_t>(opcode) << 26) |
		((static_cast<uint32_t>(rd) & 0x0Fu) << 22) |
		((static_cast<uint32_t>(rs1) & 0x0Fu) << 18) |
		mask_imm18(imm18);
}

constexpr uint32_t encode_s(Opcode opcode, uint8_t rs2, uint8_t rs1, int32_t imm18) {
	return (static_cast<uint32_t>(opcode) << 26) |
		((static_cast<uint32_t>(rs2) & 0x0Fu) << 22) |
		((static_cast<uint32_t>(rs1) & 0x0Fu) << 18) |
		mask_imm18(imm18);
}

constexpr uint32_t encode_b(Opcode opcode, uint8_t rs1, uint8_t rs2, int32_t byte_offset) {
	return (static_cast<uint32_t>(opcode) << 26) |
		((static_cast<uint32_t>(rs1) & 0x0Fu) << 22) |
		((static_cast<uint32_t>(rs2) & 0x0Fu) << 18) |
		mask_imm18(byte_offset >> 2);
}

constexpr uint32_t encode_j(Opcode opcode, int32_t byte_offset) {
	return (static_cast<uint32_t>(opcode) << 26) | mask_imm26(byte_offset >> 2);
}

constexpr uint32_t encode_x(Opcode opcode, uint8_t rd_or_rs, uint8_t rs1, uint8_t rs2, uint16_t subop_or_imm14) {
	return encode_r(opcode, rd_or_rs, rs1, rs2, subop_or_imm14);
}

constexpr uint32_t encode_movi(uint8_t rd, int32_t imm18) { return encode_i(Opcode::MOVI, rd, 0u, imm18); }
constexpr uint32_t encode_movhi(uint8_t rd, int32_t imm18) { return encode_i(Opcode::MOVHI, rd, 0u, imm18); }
constexpr uint32_t encode_mov(uint8_t rd, uint8_t rs1) { return encode_r(Opcode::MOV, rd, rs1, 0u); }
constexpr uint32_t encode_add(uint8_t rd, uint8_t rs1, uint8_t rs2) { return encode_r(Opcode::ADD, rd, rs1, rs2); }
constexpr uint32_t encode_addi(uint8_t rd, uint8_t rs1, int32_t imm18) { return encode_i(Opcode::ADDI, rd, rs1, imm18); }
constexpr uint32_t encode_sub(uint8_t rd, uint8_t rs1, uint8_t rs2) { return encode_r(Opcode::SUB, rd, rs1, rs2); }
constexpr uint32_t encode_and(uint8_t rd, uint8_t rs1, uint8_t rs2) { return encode_r(Opcode::AND, rd, rs1, rs2); }
constexpr uint32_t encode_or(uint8_t rd, uint8_t rs1, uint8_t rs2) { return encode_r(Opcode::OR, rd, rs1, rs2); }
constexpr uint32_t encode_xor(uint8_t rd, uint8_t rs1, uint8_t rs2) { return encode_r(Opcode::XOR, rd, rs1, rs2); }
constexpr uint32_t encode_shl(uint8_t rd, uint8_t rs1, uint8_t imm5) { return encode_i(Opcode::SHL, rd, rs1, imm5 & 0x1Fu); }
constexpr uint32_t encode_shr(uint8_t rd, uint8_t rs1, uint8_t imm5) { return encode_i(Opcode::SHR, rd, rs1, imm5 & 0x1Fu); }
constexpr uint32_t encode_cmp(uint8_t rs1, uint8_t rs2) { return encode_r(Opcode::CMP, 0u, rs1, rs2); }
constexpr uint32_t encode_cmpi(uint8_t rs1, int32_t imm18) { return encode_i(Opcode::CMPI, 0u, rs1, imm18); }
constexpr uint32_t encode_b(int32_t byte_offset) { return encode_j(Opcode::B, byte_offset); }
constexpr uint32_t encode_beq(uint8_t rs1, uint8_t rs2, int32_t byte_offset) { return encode_b(Opcode::BEQ, rs1, rs2, byte_offset); }
constexpr uint32_t encode_bne(uint8_t rs1, uint8_t rs2, int32_t byte_offset) { return encode_b(Opcode::BNE, rs1, rs2, byte_offset); }
constexpr uint32_t encode_blt(uint8_t rs1, uint8_t rs2, int32_t byte_offset) { return encode_b(Opcode::BLT, rs1, rs2, byte_offset); }
constexpr uint32_t encode_bge(uint8_t rs1, uint8_t rs2, int32_t byte_offset) { return encode_b(Opcode::BGE, rs1, rs2, byte_offset); }
constexpr uint32_t encode_call(int32_t byte_offset) { return encode_j(Opcode::CALL, byte_offset); }
constexpr uint32_t encode_callr(uint8_t rs1) { return encode_r(Opcode::CALLR, 0u, rs1, 0u); }
constexpr uint32_t encode_ret() { return encode_r(Opcode::RET, 0u, 0u, 0u); }
constexpr uint32_t encode_nop() { return encode_r(Opcode::NOP, 0u, 0u, 0u); }
constexpr uint32_t encode_hlt() { return encode_r(Opcode::HLT, 0u, 0u, 0u); }
constexpr uint32_t encode_ldw(uint8_t rd, uint8_t rs1, int32_t imm18) { return encode_i(Opcode::LDW, rd, rs1, imm18); }
constexpr uint32_t encode_stw(uint8_t rs2, uint8_t rs1, int32_t imm18) { return encode_s(Opcode::STW, rs2, rs1, imm18); }
constexpr uint32_t encode_ldb(uint8_t rd, uint8_t rs1, int32_t imm18) { return encode_i(Opcode::LDB, rd, rs1, imm18); }
constexpr uint32_t encode_stb(uint8_t rs2, uint8_t rs1, int32_t imm18) { return encode_s(Opcode::STB, rs2, rs1, imm18); }
constexpr uint32_t encode_push(uint8_t rs1) { return encode_r(Opcode::PUSH, 0u, rs1, 0u); }
constexpr uint32_t encode_pop(uint8_t rd) { return encode_r(Opcode::POP, rd, 0u, 0u); }
constexpr uint32_t encode_csrrd(uint8_t rd, uint32_t csr_id) { return encode_i(Opcode::CSRRD, rd, 0u, static_cast<int32_t>(csr_id)); }
constexpr uint32_t encode_csrwr(uint32_t csr_id, uint8_t rs1) { return encode_i(Opcode::CSRWR, 0u, rs1, static_cast<int32_t>(csr_id)); }
constexpr uint32_t encode_csrsi(uint32_t csr_id, uint8_t rs1_mask) { return encode_x(Opcode::CSRSI, 0u, rs1_mask, 0u, static_cast<uint16_t>(csr_id & 0x3FFFu)); }
constexpr uint32_t encode_csrcl(uint32_t csr_id, uint8_t rs1_mask) { return encode_x(Opcode::CSRCL, 0u, rs1_mask, 0u, static_cast<uint16_t>(csr_id & 0x3FFFu)); }
constexpr uint32_t encode_wfi(uint8_t rs1) { return encode_r(Opcode::WFI, 0u, rs1, 0u); }
constexpr uint32_t encode_wait(uint8_t rd, uint8_t rs1) { return encode_r(Opcode::WAIT, rd, rs1, 0u); }
constexpr uint32_t encode_ackirq(uint8_t rs1) { return encode_r(Opcode::ACKIRQ, 0u, rs1, 0u); }
constexpr uint32_t encode_ei() { return encode_r(Opcode::EI, 0u, 0u, 0u); }
constexpr uint32_t encode_di() { return encode_r(Opcode::DI, 0u, 0u, 0u); }
constexpr uint32_t encode_iret() { return encode_r(Opcode::IRET, 0u, 0u, 0u); }

constexpr uint32_t encode_csr_rd(uint8_t rd, uint32_t csr_id) { return encode_csrrd(rd, csr_id); }
constexpr uint32_t encode_csr_wr(uint32_t csr_id, uint8_t rs1) { return encode_csrwr(csr_id, rs1); }
constexpr uint32_t encode_brz(int32_t byte_offset) { return encode_beq(0u, 0u, byte_offset); }
constexpr uint32_t encode_hcfg(uint8_t rs1) { return encode_mov(15u, rs1); }
constexpr uint32_t encode_hrun(uint8_t rs1, uint8_t rs2) { return encode_add(15u, rs1, rs2); }
constexpr uint32_t encode_hwait(uint8_t rs1) { return encode_wait(15u, rs1); }
constexpr uint32_t encode_hpoll(uint8_t rd) { return encode_csrrd(rd, kCsrHaccStatus); }
constexpr uint32_t encode_herr(uint8_t rd) { return encode_csrrd(rd, kCsrHaccErrCode); }
constexpr uint32_t encode_hbreak() { return encode_hlt(); }

} // namespace hybridacc::core