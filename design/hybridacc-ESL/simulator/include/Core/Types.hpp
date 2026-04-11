#pragma once

/**
 * @file Types.hpp
 * @brief Shared type definitions for the HACC Core Controller subsystem.
 *
 * Defines enumerations, struct payloads, and constants used across
 * cc_core_mcu, cc_cmd_fabric, cc_dma_engine, cc_plic, cc_section_loader,
 * cc_boot_host_if, and cc_cluster_data_fabric.
 */

#include <systemc>
#include <cstdint>
#include <iostream>
#include <string>

namespace hybridacc {
namespace core {

using namespace sc_core;
using namespace sc_dt;

// ============================================================================
// Global core-controller parameters (compile-time defaults)
// ============================================================================

static constexpr unsigned kCoreXlen           = 32;
static constexpr unsigned kCoreGprNum         = 32;
static constexpr unsigned kCorePipeStages     = 5;

static constexpr unsigned kIsramBytes         = 16384; ///< 16 KB instruction SRAM
static constexpr unsigned kDataSramBytes      = 65536; ///< 64 KB data SRAM
static constexpr unsigned kBootRomBytes       = 4096;  ///< 4 KB optional boot ROM

static constexpr unsigned kClAxiDataWidth     = 64;    ///< cluster data port width
static constexpr unsigned kClAhbDataWidth     = 32;    ///< cluster command port width
static constexpr unsigned kMemAxiDataWidth    = 64;   ///< external DRAM AXI4 width

static constexpr unsigned kDmaCmdFifoDepth    = 8;

// ============================================================================
// Address-space base addresses (core-visible flat address map)
// ============================================================================

static constexpr uint32_t kBaseInstRam        = 0x0000'0000;
static constexpr uint32_t kEndInstRam         = 0x0000'3FFF;
static constexpr uint32_t kBaseBootRom        = 0x0001'0000;
static constexpr uint32_t kEndBootRom         = 0x0001'0FFF;
static constexpr uint32_t kBaseDataRam        = 0x1000'0000;
static constexpr uint32_t kEndDataRam         = 0x1000'FFFF;
static constexpr uint32_t kBaseLocalCtrl      = 0x2000'0000;
static constexpr uint32_t kEndLocalCtrl       = 0x2000'0FFF;
static constexpr uint32_t kBaseDmaMmio        = 0x2000'1000;
static constexpr uint32_t kEndDmaMmio         = 0x2000'17FF;
static constexpr uint32_t kBaseLocalTimer     = 0x2000'2000;
static constexpr uint32_t kEndLocalTimer      = 0x2000'20FF;
static constexpr uint32_t kBasePlic           = 0x0C00'0000;
static constexpr uint32_t kEndPlic            = 0x0C00'FFFF;
static constexpr uint32_t kBaseClusterUnicast = 0x4000'0000;
static constexpr uint32_t kEndClusterUnicast  = 0x400F'FFFF;
static constexpr uint32_t kBaseClusterBcast   = 0x5000'0000;
static constexpr uint32_t kEndClusterBcast    = 0x5000'FFFF;
static constexpr uint32_t kBaseNlu            = 0x6000'0000;
static constexpr uint32_t kEndNlu             = 0x6000'FFFF;

static constexpr uint32_t kClusterStride      = 0x0001'0000;
static constexpr uint32_t kNluStride          = 0x0000'1000;

// ============================================================================
// Section-kind encoding (manifest)
// ============================================================================

enum class SectionKind : uint16_t {
    CORE    = 0x0001,
    JOB     = 0x0002,
    BLOCK   = 0x0003,
    PROFILE = 0x0004,
    DMA     = 0x0005,
    AGU     = 0x0006,
    NLU     = 0x0007,
    PE      = 0x0008,
    SCAN    = 0x0009,
    PATCH   = 0x000A,
    DEBUG   = 0x000B,
};

inline std::ostream& operator<<(std::ostream& os, SectionKind k) {
    switch (k) {
        case SectionKind::CORE:    return os << "CORE";
        case SectionKind::JOB:     return os << "JOB";
        case SectionKind::BLOCK:   return os << "BLOCK";
        case SectionKind::PROFILE: return os << "PROFILE";
        case SectionKind::DMA:     return os << "DMA";
        case SectionKind::AGU:     return os << "AGU";
        case SectionKind::NLU:     return os << "NLU";
        case SectionKind::PE:      return os << "PE";
        case SectionKind::SCAN:    return os << "SCAN";
        case SectionKind::PATCH:   return os << "PATCH";
        case SectionKind::DEBUG:   return os << "DEBUG";
        default:                   return os << "UNKNOWN_SECTION";
    }
}

inline void sc_trace(sc_trace_file* tf, const SectionKind& k, const std::string& name) {
    sc_trace(tf, static_cast<uint16_t>(k), name);
}

// ============================================================================
// Manifest entry (32 bytes = 8 words)
// ============================================================================

struct ManifestEntry {
    uint16_t section_kind;
    uint16_t flags;
    uint32_t dram_addr_lo;
    uint32_t dram_addr_hi;
    uint32_t local_addr;
    uint32_t size_bytes;
    uint32_t crc32;
    uint32_t attr0;
    uint32_t reserved;

    bool operator==(const ManifestEntry& o) const {
        return section_kind == o.section_kind && flags == o.flags &&
               dram_addr_lo == o.dram_addr_lo && dram_addr_hi == o.dram_addr_hi &&
               local_addr == o.local_addr && size_bytes == o.size_bytes &&
               crc32 == o.crc32 && attr0 == o.attr0;
    }
    friend std::ostream& operator<<(std::ostream& os, const ManifestEntry& e) {
        os << "ManifestEntry{kind=0x" << std::hex << e.section_kind
           << ", flags=0x" << e.flags
           << ", dram=0x" << e.dram_addr_hi << e.dram_addr_lo
           << ", local=0x" << e.local_addr
           << ", size=" << std::dec << e.size_bytes << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const ManifestEntry& e, const std::string& name) {
        sc_trace(tf, e.section_kind, name + ".section_kind");
        sc_trace(tf, e.local_addr,   name + ".local_addr");
        sc_trace(tf, e.size_bytes,   name + ".size_bytes");
    }
};

// ============================================================================
// Loader error codes
// ============================================================================

enum class LoaderError : uint32_t {
    LD_OK                   = 0,
    LD_ERR_MANIFEST_SIZE    = 1,
    LD_ERR_BAD_SECTION_KIND = 2,
    LD_ERR_LOCAL_ADDR_OOB   = 3,
    LD_ERR_SIZE_OOB         = 4,
    LD_ERR_CRC_MISMATCH     = 5,
    LD_ERR_AXI              = 6,
    LD_ERR_OVERLAP          = 7,
};

inline std::ostream& operator<<(std::ostream& os, LoaderError e) {
    switch (e) {
        case LoaderError::LD_OK:                   return os << "LD_OK";
        case LoaderError::LD_ERR_MANIFEST_SIZE:    return os << "LD_ERR_MANIFEST_SIZE";
        case LoaderError::LD_ERR_BAD_SECTION_KIND: return os << "LD_ERR_BAD_SECTION_KIND";
        case LoaderError::LD_ERR_LOCAL_ADDR_OOB:   return os << "LD_ERR_LOCAL_ADDR_OOB";
        case LoaderError::LD_ERR_SIZE_OOB:         return os << "LD_ERR_SIZE_OOB";
        case LoaderError::LD_ERR_CRC_MISMATCH:     return os << "LD_ERR_CRC_MISMATCH";
        case LoaderError::LD_ERR_AXI:              return os << "LD_ERR_AXI";
        case LoaderError::LD_ERR_OVERLAP:          return os << "LD_ERR_OVERLAP";
        default:                                   return os << "LD_ERR_UNKNOWN";
    }
}

inline void sc_trace(sc_trace_file* tf, const LoaderError& e, const std::string& name) {
    sc_trace(tf, static_cast<uint32_t>(e), name);
}

// ============================================================================
// DMA error codes
// ============================================================================

enum class DmaError : uint32_t {
    DMA_ERR_NONE            = 0,
    DMA_ERR_SUBMIT_WHEN_FULL = 1,
    DMA_ERR_BAD_ENDPOINT    = 2,
    DMA_ERR_ADDR_ALIGN      = 3,
    DMA_ERR_ZERO_LENGTH     = 4,
    DMA_ERR_CLUSTER_RESP    = 5,
    DMA_ERR_DRAM_AXI        = 6,
    DMA_ERR_ABORTED         = 7,
};

inline std::ostream& operator<<(std::ostream& os, DmaError e) {
    switch (e) {
        case DmaError::DMA_ERR_NONE:            return os << "DMA_ERR_NONE";
        case DmaError::DMA_ERR_SUBMIT_WHEN_FULL:return os << "DMA_ERR_SUBMIT_WHEN_FULL";
        case DmaError::DMA_ERR_BAD_ENDPOINT:    return os << "DMA_ERR_BAD_ENDPOINT";
        case DmaError::DMA_ERR_ADDR_ALIGN:      return os << "DMA_ERR_ADDR_ALIGN";
        case DmaError::DMA_ERR_ZERO_LENGTH:     return os << "DMA_ERR_ZERO_LENGTH";
        case DmaError::DMA_ERR_CLUSTER_RESP:    return os << "DMA_ERR_CLUSTER_RESP";
        case DmaError::DMA_ERR_DRAM_AXI:        return os << "DMA_ERR_DRAM_AXI";
        case DmaError::DMA_ERR_ABORTED:         return os << "DMA_ERR_ABORTED";
        default:                                return os << "DMA_ERR_UNKNOWN";
    }
}

inline void sc_trace(sc_trace_file* tf, const DmaError& e, const std::string& name) {
    sc_trace(tf, static_cast<uint32_t>(e), name);
}

// ============================================================================
// DMA operation / endpoint kind
// ============================================================================

enum class DmaEndpoint : uint32_t {
    DRAM        = 0,
    CLUSTER_SPM = 1,
};

inline std::ostream& operator<<(std::ostream& os, DmaEndpoint k) {
    switch (k) {
        case DmaEndpoint::DRAM:        return os << "DRAM";
        case DmaEndpoint::CLUSTER_SPM: return os << "CLUSTER_SPM";
        default:                       return os << "DMA_EP_UNKNOWN";
    }
}

inline void sc_trace(sc_trace_file* tf, const DmaEndpoint& k, const std::string& name) {
    sc_trace(tf, static_cast<uint32_t>(k), name);
}

// ============================================================================
// DMA command (snapshot into command FIFO)
// ============================================================================

struct DmaCommand {
    DmaEndpoint src_kind;
    DmaEndpoint dst_kind;
    uint32_t src_addr_lo;
    uint32_t src_addr_hi;
    uint32_t dst_addr_lo;
    uint32_t dst_addr_hi;
    uint32_t src_cluster_id;
    uint32_t dst_cluster_id;
    uint32_t count[4];       ///< 4D iteration counts (d0=innermost)
    uint32_t src_stride[4];  ///< source stride per dimension (bytes)
    uint32_t dst_stride[4];  ///< destination stride per dimension (bytes)
    uint32_t cmd_tag;

    /// Total number of beats = product of all counts (0 treated as 1).
    uint32_t total_beats() const {
        uint32_t n = 1;
        for (int i = 0; i < 4; ++i)
            n *= (count[i] == 0) ? 1 : count[i];
        return n;
    }

    bool operator==(const DmaCommand& o) const {
        return src_kind == o.src_kind && dst_kind == o.dst_kind &&
               src_addr_lo == o.src_addr_lo && dst_addr_lo == o.dst_addr_lo &&
               total_beats() == o.total_beats() && cmd_tag == o.cmd_tag;
    }
    friend std::ostream& operator<<(std::ostream& os, const DmaCommand& c) {
        os << "DmaCmd{src=" << c.src_kind
           << ", dst=" << c.dst_kind
           << ", beats=" << c.total_beats()
           << ", tag=" << c.cmd_tag << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const DmaCommand& c, const std::string& name) {
        sc_trace(tf, c.cmd_tag,     name + ".cmd_tag");
        sc_trace(tf, c.count[0],    name + ".count_d0");
        sc_trace(tf, c.src_addr_lo, name + ".src_addr_lo");
    }
};

// ============================================================================
// MMIO request / response (core <-> cmd_fabric)
// ============================================================================

struct MmioRequest {
    bool     write;
    uint32_t addr;
    uint32_t wdata;
    uint32_t wstrb;

    bool operator==(const MmioRequest& o) const {
        return write == o.write && addr == o.addr &&
               wdata == o.wdata && wstrb == o.wstrb;
    }
    friend std::ostream& operator<<(std::ostream& os, const MmioRequest& r) {
        os << "MmioReq{" << (r.write ? "W" : "R")
           << ", addr=0x" << std::hex << r.addr
           << ", data=0x" << r.wdata << std::dec << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const MmioRequest& r, const std::string& name) {
        sc_trace(tf, r.write, name + ".write");
        sc_trace(tf, r.addr,  name + ".addr");
        sc_trace(tf, r.wdata, name + ".wdata");
    }
};

struct MmioResponse {
    uint32_t rdata;
    bool     error;

    bool operator==(const MmioResponse& o) const {
        return rdata == o.rdata && error == o.error;
    }
    friend std::ostream& operator<<(std::ostream& os, const MmioResponse& r) {
        os << "MmioResp{data=0x" << std::hex << r.rdata
           << ", err=" << std::dec << r.error << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const MmioResponse& r, const std::string& name) {
        sc_trace(tf, r.rdata, name + ".rdata");
        sc_trace(tf, r.error, name + ".error");
    }
};

// ============================================================================
// Cluster data fabric request / response
// ============================================================================

struct ClusterDataRequest {
    bool     write;
    uint32_t cluster_id;
    uint32_t addr;
    uint64_t wdata;
    uint8_t  wstrb;

    bool operator==(const ClusterDataRequest& o) const {
        return write == o.write && cluster_id == o.cluster_id &&
               addr == o.addr && wdata == o.wdata && wstrb == o.wstrb;
    }
    friend std::ostream& operator<<(std::ostream& os, const ClusterDataRequest& r) {
        os << "ClDataReq{" << (r.write ? "W" : "R")
           << ", cl=" << r.cluster_id
           << ", addr=0x" << std::hex << r.addr
           << ", data=0x" << r.wdata << std::dec << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const ClusterDataRequest& r, const std::string& name) {
        sc_trace(tf, r.write,      name + ".write");
        sc_trace(tf, r.cluster_id, name + ".cluster_id");
        sc_trace(tf, r.addr,       name + ".addr");
    }
};

struct ClusterDataResponse {
    uint64_t rdata;
    bool     error;

    bool operator==(const ClusterDataResponse& o) const {
        return rdata == o.rdata && error == o.error;
    }
    friend std::ostream& operator<<(std::ostream& os, const ClusterDataResponse& r) {
        os << "ClDataResp{data=0x" << std::hex << r.rdata
           << ", err=" << std::dec << r.error << "}";
        return os;
    }
    friend void sc_trace(sc_trace_file* tf, const ClusterDataResponse& r, const std::string& name) {
        sc_trace(tf, r.rdata, name + ".rdata");
        sc_trace(tf, r.error, name + ".error");
    }
};

// ============================================================================
// PLIC source id helpers
// ============================================================================

/**
 * @brief Compute the total number of PLIC sources.
 *
 *   source 0           = reserved
 *   1..NUM_CLUSTERS     = cluster_irq
 *   NUM_CLUSTERS+1      = dma_irq
 *   NUM_CLUSTERS+2..+NLU+1 = nlu_irq
 *   +NLU+2              = loader_fault
 *   +NLU+3              = fabric_fault
 */
static constexpr unsigned plic_num_sources(unsigned num_cl, unsigned num_nlu) {
    return num_cl + num_nlu + 3;
}

// ============================================================================
// RV32I CSR addresses used by the core
// ============================================================================

static constexpr uint32_t kCsrMstatus  = 0x300;
static constexpr uint32_t kCsrMisa     = 0x301;
static constexpr uint32_t kCsrMie      = 0x304;
static constexpr uint32_t kCsrMtvec    = 0x305;
static constexpr uint32_t kCsrMscratch = 0x340;
static constexpr uint32_t kCsrMepc     = 0x341;
static constexpr uint32_t kCsrMcause   = 0x342;
static constexpr uint32_t kCsrMtval    = 0x343;
static constexpr uint32_t kCsrMip      = 0x344;
static constexpr uint32_t kCsrMcycle   = 0xB00;
static constexpr uint32_t kCsrMinstret = 0xB02;

// ============================================================================
// Core local control MMIO offsets
// ============================================================================

static constexpr uint32_t kLocalClusterMaskLo = 0x000;
static constexpr uint32_t kLocalClusterMaskHi = 0x004;
static constexpr uint32_t kLocalMmioErrStatus = 0x008;
static constexpr uint32_t kLocalLastTargetId  = 0x00C;
static constexpr uint32_t kLocalLastFaultAddr = 0x010;
static constexpr uint32_t kLocalLastFaultInfo = 0x014;
static constexpr uint32_t kLocalBootReason    = 0x018;
static constexpr uint32_t kLocalFabricCap0    = 0x01C;

// ============================================================================
// DMA MMIO offsets
// ============================================================================

static constexpr uint32_t kDmaCap0          = 0x000;
static constexpr uint32_t kDmaStatus        = 0x004;
static constexpr uint32_t kDmaCtrl          = 0x008;
static constexpr uint32_t kDmaSrcKind       = 0x00C;
static constexpr uint32_t kDmaDstKind       = 0x010;
static constexpr uint32_t kDmaSrcAddrLo     = 0x014;
static constexpr uint32_t kDmaSrcAddrHi     = 0x018;
static constexpr uint32_t kDmaDstAddrLo     = 0x01C;
static constexpr uint32_t kDmaDstAddrHi     = 0x020;
static constexpr uint32_t kDmaSrcClusterId  = 0x024;
static constexpr uint32_t kDmaDstClusterId  = 0x028;
static constexpr uint32_t kDmaCountD0       = 0x02C;
static constexpr uint32_t kDmaCountD1       = 0x030;
static constexpr uint32_t kDmaCountD2       = 0x034;
static constexpr uint32_t kDmaCountD3       = 0x038;
static constexpr uint32_t kDmaSrcStrideD0   = 0x03C;
static constexpr uint32_t kDmaSrcStrideD1   = 0x040;
static constexpr uint32_t kDmaSrcStrideD2   = 0x044;
static constexpr uint32_t kDmaSrcStrideD3   = 0x048;
static constexpr uint32_t kDmaDstStrideD0   = 0x04C;
static constexpr uint32_t kDmaDstStrideD1   = 0x050;
static constexpr uint32_t kDmaDstStrideD2   = 0x054;
static constexpr uint32_t kDmaDstStrideD3   = 0x058;
static constexpr uint32_t kDmaCmdTag        = 0x05C;
static constexpr uint32_t kDmaDoneTag       = 0x060;
static constexpr uint32_t kDmaErrCode       = 0x064;
static constexpr uint32_t kDmaErrInfo       = 0x068;
static constexpr uint32_t kDmaDebugState    = 0x06C;

// ============================================================================
// PLIC MMIO offsets
// ============================================================================

static constexpr uint32_t kPlicPriorityBase = 0x0000;
static constexpr uint32_t kPlicPendingLo    = 0x0800;
static constexpr uint32_t kPlicPendingHi    = 0x0804;
static constexpr uint32_t kPlicEnableLo     = 0x1000;
static constexpr uint32_t kPlicEnableHi     = 0x1004;
static constexpr uint32_t kPlicThreshold    = 0x1800;
static constexpr uint32_t kPlicClaimComplete= 0x1804;
static constexpr uint32_t kPlicMaxSourceId  = 0x1808;

// ============================================================================
// Core local timer MMIO offsets
// ============================================================================

static constexpr uint32_t kTimerMsip       = 0x000;
static constexpr uint32_t kTimerMtimecmpLo = 0x004;
static constexpr uint32_t kTimerMtimecmpHi = 0x008;
static constexpr uint32_t kTimerMtimeLo    = 0x00C;
static constexpr uint32_t kTimerMtimeHi    = 0x010;
static constexpr uint32_t kTimerCtrl       = 0x014;

} // namespace core
} // namespace hybridacc
