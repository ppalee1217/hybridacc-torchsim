//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   core_pkg
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Core-controller shared constants and lightweight payloads.
//                Mirrors the ESL contract in Core/Types.hpp.
// Dependencies:  None
// Revision:
//   2026/04/27 - Initial version (M3 core contract baseline)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`ifndef CORE_PKG_SV
`define CORE_PKG_SV

package core_pkg;

    localparam int unsigned CORE_XLEN        = 32;
    localparam int unsigned CORE_GPR_NUM     = 32;
    localparam int unsigned CORE_PIPE_STAGES = 5;

    localparam int unsigned ISRAM_BYTES      = 65536;
    localparam int unsigned DATA_SRAM_BYTES  = 65536;
    localparam int unsigned BOOT_ROM_BYTES   = 4096;

    localparam int unsigned CL_AXI_DATA_WIDTH = 64;
    localparam int unsigned CL_AHB_DATA_WIDTH = 32;
    localparam int unsigned MEM_AXI_DATA_WIDTH = 64;
    localparam int unsigned DMA_CMD_FIFO_DEPTH = 8;
    localparam int unsigned DMA_MAX_OUTSTANDING = 16;

    localparam logic [31:0] BASE_INST_RAM         = 32'h0000_0000;
    localparam logic [31:0] END_INST_RAM          = 32'h0000_3FFF;
    localparam logic [31:0] BASE_BOOT_ROM         = 32'h0001_0000;
    localparam logic [31:0] END_BOOT_ROM          = 32'h0001_0FFF;
    localparam logic [31:0] BASE_DATA_RAM         = 32'h1000_0000;
    localparam logic [31:0] END_DATA_RAM          = 32'h1000_FFFF;
    localparam logic [31:0] BASE_LOCAL_CTRL       = 32'h2000_0000;
    localparam logic [31:0] END_LOCAL_CTRL        = 32'h2000_0FFF;
    localparam logic [31:0] BASE_DMA_MMIO         = 32'h2000_1000;
    localparam logic [31:0] END_DMA_MMIO          = 32'h2000_17FF;
    localparam logic [31:0] BASE_LOCAL_TIMER      = 32'h2000_2000;
    localparam logic [31:0] END_LOCAL_TIMER       = 32'h2000_20FF;
    localparam logic [31:0] BASE_PLIC             = 32'h0C00_0000;
    localparam logic [31:0] END_PLIC              = 32'h0C00_FFFF;
    localparam logic [31:0] BASE_CLUSTER_UNICAST  = 32'h4000_0000;
    localparam logic [31:0] END_CLUSTER_UNICAST   = 32'h400F_FFFF;
    localparam logic [31:0] BASE_CLUSTER_BCAST    = 32'h5000_0000;
    localparam logic [31:0] END_CLUSTER_BCAST     = 32'h5000_FFFF;
    localparam logic [31:0] BASE_NLU              = 32'h6000_0000;
    localparam logic [31:0] END_NLU               = 32'h6000_FFFF;

    localparam logic [31:0] CLUSTER_STRIDE        = 32'h0001_0000;
    localparam logic [31:0] NLU_STRIDE            = 32'h0000_1000;

    localparam logic [31:0] HACC_CAP0             = 32'h0000;
    localparam logic [31:0] HACC_CAP1             = 32'h0004;
    localparam logic [31:0] HACC_CTRL             = 32'h0008;
    localparam logic [31:0] HACC_STATUS           = 32'h000C;
    localparam logic [31:0] CORE_BOOT_ADDR        = 32'h0010;
    localparam logic [31:0] CORE_TRAP_VECTOR      = 32'h0014;
    localparam logic [31:0] CORE_PC_SNAPSHOT      = 32'h0018;
    localparam logic [31:0] CORE_CAUSE_SNAPSHOT   = 32'h001C;
    localparam logic [31:0] MANIFEST_ADDR_LO      = 32'h0020;
    localparam logic [31:0] MANIFEST_ADDR_HI      = 32'h0024;
    localparam logic [31:0] MANIFEST_SIZE         = 32'h0028;
    localparam logic [31:0] MANIFEST_KICK         = 32'h002C;
    localparam logic [31:0] LOADER_STATUS         = 32'h0030;
    localparam logic [31:0] LOADER_ERR_CODE       = 32'h0034;
    localparam logic [31:0] LOADER_ERR_INFO       = 32'h0038;
    localparam logic [31:0] IRQ_SUMMARY           = 32'h0040;
    localparam logic [31:0] IRQ_FORCE_ACK         = 32'h0044;
    localparam logic [31:0] CLUSTER_MASK_LO       = 32'h0050;
    localparam logic [31:0] CLUSTER_MASK_HI       = 32'h0054;
    localparam logic [31:0] LAST_MMIO_TARGET      = 32'h0058;
    localparam logic [31:0] LAST_MMIO_ADDR        = 32'h005C;
    localparam logic [31:0] TRACE_BASE            = 32'h0060;
    localparam logic [31:0] TRACE_SIZE            = 32'h0064;
    localparam logic [31:0] TRACE_CTRL            = 32'h0068;
    localparam logic [31:0] TRACE_STATUS          = 32'h006C;

    localparam logic [31:0] LOCAL_CLUSTER_MASK_LO = 32'h000;
    localparam logic [31:0] LOCAL_CLUSTER_MASK_HI = 32'h004;
    localparam logic [31:0] LOCAL_MMIO_ERR_STATUS = 32'h008;
    localparam logic [31:0] LOCAL_LAST_TARGET_ID  = 32'h00C;
    localparam logic [31:0] LOCAL_LAST_FAULT_ADDR = 32'h010;
    localparam logic [31:0] LOCAL_LAST_FAULT_INFO = 32'h014;
    localparam logic [31:0] LOCAL_BOOT_REASON     = 32'h018;
    localparam logic [31:0] LOCAL_FABRIC_CAP0     = 32'h01C;

    localparam logic [31:0] DMA_CAP0          = 32'h000;
    localparam logic [31:0] DMA_STATUS        = 32'h004;
    localparam logic [31:0] DMA_CTRL          = 32'h008;
    localparam logic [31:0] DMA_SRC_KIND      = 32'h00C;
    localparam logic [31:0] DMA_DST_KIND      = 32'h010;
    localparam logic [31:0] DMA_SRC_ADDR_LO   = 32'h014;
    localparam logic [31:0] DMA_SRC_ADDR_HI   = 32'h018;
    localparam logic [31:0] DMA_DST_ADDR_LO   = 32'h01C;
    localparam logic [31:0] DMA_DST_ADDR_HI   = 32'h020;
    localparam logic [31:0] DMA_SRC_CLUSTER_ID= 32'h024;
    localparam logic [31:0] DMA_DST_CLUSTER_ID= 32'h028;
    localparam logic [31:0] DMA_COUNT_D0      = 32'h02C;
    localparam logic [31:0] DMA_COUNT_D1      = 32'h030;
    localparam logic [31:0] DMA_COUNT_D2      = 32'h034;
    localparam logic [31:0] DMA_COUNT_D3      = 32'h038;
    localparam logic [31:0] DMA_SRC_STRIDE_D0 = 32'h03C;
    localparam logic [31:0] DMA_SRC_STRIDE_D1 = 32'h040;
    localparam logic [31:0] DMA_SRC_STRIDE_D2 = 32'h044;
    localparam logic [31:0] DMA_SRC_STRIDE_D3 = 32'h048;
    localparam logic [31:0] DMA_DST_STRIDE_D0 = 32'h04C;
    localparam logic [31:0] DMA_DST_STRIDE_D1 = 32'h050;
    localparam logic [31:0] DMA_DST_STRIDE_D2 = 32'h054;
    localparam logic [31:0] DMA_DST_STRIDE_D3 = 32'h058;
    localparam logic [31:0] DMA_CMD_TAG       = 32'h05C;
    localparam logic [31:0] DMA_DONE_TAG      = 32'h060;
    localparam logic [31:0] DMA_ERR_CODE      = 32'h064;
    localparam logic [31:0] DMA_ERR_INFO      = 32'h068;
    localparam logic [31:0] DMA_DEBUG_STATE   = 32'h06C;

    localparam logic [31:0] PLIC_PRIORITY_BASE = 32'h0000;
    localparam logic [31:0] PLIC_PENDING_LO    = 32'h0800;
    localparam logic [31:0] PLIC_PENDING_HI    = 32'h0804;
    localparam logic [31:0] PLIC_ENABLE_LO     = 32'h1000;
    localparam logic [31:0] PLIC_ENABLE_HI     = 32'h1004;
    localparam logic [31:0] PLIC_THRESHOLD     = 32'h1800;
    localparam logic [31:0] PLIC_CLAIM_COMPLETE= 32'h1804;
    localparam logic [31:0] PLIC_MAX_SOURCE_ID = 32'h1808;

    localparam logic [31:0] TIMER_MSIP         = 32'h000;
    localparam logic [31:0] TIMER_MTIMECMP_LO  = 32'h004;
    localparam logic [31:0] TIMER_MTIMECMP_HI  = 32'h008;
    localparam logic [31:0] TIMER_MTIME_LO     = 32'h00C;
    localparam logic [31:0] TIMER_MTIME_HI     = 32'h010;
    localparam logic [31:0] TIMER_CTRL         = 32'h014;

    localparam logic [11:0] CSR_MSTATUS  = 12'h300;
    localparam logic [11:0] CSR_MISA     = 12'h301;
    localparam logic [11:0] CSR_MIE      = 12'h304;
    localparam logic [11:0] CSR_MTVEC    = 12'h305;
    localparam logic [11:0] CSR_MSCRATCH = 12'h340;
    localparam logic [11:0] CSR_MEPC     = 12'h341;
    localparam logic [11:0] CSR_MCAUSE   = 12'h342;
    localparam logic [11:0] CSR_MTVAL    = 12'h343;
    localparam logic [11:0] CSR_MIP      = 12'h344;
    localparam logic [11:0] CSR_MCYCLE   = 12'hB00;
    localparam logic [11:0] CSR_MINSTRET = 12'hB02;

    localparam logic [31:0] DMA_XFORM_LOAD_PAD_EN      = 32'h0000_0001;
    localparam int unsigned DMA_XFORM_FILL_MODE_SHIFT  = 4;
    localparam logic [31:0] DMA_XFORM_FILL_MODE_MASK   = 32'h0000_0030;
    localparam logic [31:0] DMA_EPILOGUE_MODE_MASK     = 32'h0000_0003;

    typedef enum logic [15:0] {
        SECTION_CORE    = 16'h0001,
        SECTION_JOB     = 16'h0002,
        SECTION_BLOCK   = 16'h0003,
        SECTION_PROFILE = 16'h0004,
        SECTION_DMA     = 16'h0005,
        SECTION_AGU     = 16'h0006,
        SECTION_NLU     = 16'h0007,
        SECTION_PE      = 16'h0008,
        SECTION_SCAN    = 16'h0009,
        SECTION_PATCH   = 16'h000A,
        SECTION_DEBUG   = 16'h000B
    } section_kind_e;

    typedef struct packed {
        logic [15:0] section_kind;
        logic [15:0] flags;
        logic [31:0] dram_addr_lo;
        logic [31:0] dram_addr_hi;
        logic [31:0] local_addr;
        logic [31:0] size_bytes;
        logic [31:0] crc32;
        logic [31:0] attr0;
        logic [31:0] reserved;
    } manifest_entry_t;

    typedef enum logic [31:0] {
        LOADER_OK                   = 32'd0,
        LOADER_ERR_MANIFEST_SIZE    = 32'd1,
        LOADER_ERR_BAD_SECTION_KIND = 32'd2,
        LOADER_ERR_LOCAL_ADDR_OOB   = 32'd3,
        LOADER_ERR_SIZE_OOB         = 32'd4,
        LOADER_ERR_CRC_MISMATCH     = 32'd5,
        LOADER_ERR_AXI              = 32'd6,
        LOADER_ERR_OVERLAP          = 32'd7
    } loader_error_e;

    typedef enum logic [31:0] {
        DMA_ERR_NONE             = 32'd0,
        DMA_ERR_SUBMIT_WHEN_FULL = 32'd1,
        DMA_ERR_BAD_ENDPOINT     = 32'd2,
        DMA_ERR_ADDR_ALIGN       = 32'd3,
        DMA_ERR_ZERO_LENGTH      = 32'd4,
        DMA_ERR_CLUSTER_RESP     = 32'd5,
        DMA_ERR_DRAM_AXI         = 32'd6,
        DMA_ERR_ABORTED          = 32'd7,
        DMA_ERR_BAD_XFORM        = 32'd8
    } dma_error_e;

    typedef enum logic [31:0] {
        DMA_EP_DRAM        = 32'd0,
        DMA_EP_CLUSTER_SPM = 32'd1
    } dma_endpoint_e;

    typedef enum logic [31:0] {
        DMA_FILL_ZERO    = 32'd0,
        DMA_FILL_EPSILON = 32'd1,
        DMA_FILL_CONST   = 32'd2
    } dma_fill_mode_e;

    typedef enum logic [31:0] {
        DMA_EPILOGUE_NONE = 32'd0,
        DMA_EPILOGUE_RELU = 32'd1
    } dma_epilogue_mode_e;

    typedef struct packed {
        logic [31:0] xform_ctrl;
        logic signed [31:0] pad_window_h0;
        logic signed [31:0] pad_window_w0;
        logic [31:0] pad_src_h;
        logic [31:0] pad_src_w;
        logic [31:0] beats_per_pixel;
        logic [63:0] fill_value;
        logic [31:0] epilogue_ctrl;
        logic [31:0] epilogue_param0;
    } dma_transform_cfg_t;

    typedef struct packed {
        logic [31:0] src_addr_lo;
        logic [31:0] src_addr_hi;
        logic [31:0] dst_addr_lo;
        logic [31:0] dst_addr_hi;
        logic [31:0] src_cluster_id;
        logic [31:0] dst_cluster_id;
        logic [31:0] count_d0;
        logic [31:0] count_d1;
        logic [31:0] count_d2;
        logic [31:0] count_d3;
        logic [31:0] src_stride_d0;
        logic [31:0] src_stride_d1;
        logic [31:0] src_stride_d2;
        logic [31:0] src_stride_d3;
        logic [31:0] dst_stride_d0;
        logic [31:0] dst_stride_d1;
        logic [31:0] dst_stride_d2;
        logic [31:0] dst_stride_d3;
        logic [31:0] cmd_tag;
        dma_endpoint_e src_kind;
        dma_endpoint_e dst_kind;
        dma_transform_cfg_t transform;
    } dma_command_t;

    function automatic logic addr_in_range(input logic [31:0] addr, input logic [31:0] base, input logic [31:0] end_addr);
        return (addr >= base) && (addr <= end_addr);
    endfunction

    function automatic logic [31:0] apply_wstrb32(
        input logic [31:0] current,
        input logic [31:0] wdata,
        input logic [3:0]  wstrb
    );
        logic [31:0] merged;
        merged = current;
        for (int byte_idx = 0; byte_idx < 4; byte_idx++) begin
            if (wstrb[byte_idx]) begin
                merged[byte_idx*8 +: 8] = wdata[byte_idx*8 +: 8];
            end
        end
        return merged;
    endfunction

    function automatic logic [63:0] apply_wstrb64(
        input logic [63:0] current,
        input logic [63:0] wdata,
        input logic [7:0]  wstrb
    );
        logic [63:0] merged;
        merged = current;
        for (int byte_idx = 0; byte_idx < 8; byte_idx++) begin
            if (wstrb[byte_idx]) begin
                merged[byte_idx*8 +: 8] = wdata[byte_idx*8 +: 8];
            end
        end
        return merged;
    endfunction

    function automatic int unsigned plic_num_sources(
        input int unsigned num_clusters,
        input int unsigned num_nlu
    );
        return num_clusters + num_nlu + 3;
    endfunction

endpackage : core_pkg

`endif // CORE_PKG_SV