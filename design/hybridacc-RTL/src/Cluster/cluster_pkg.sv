//-----------------------------------------------------------------------------
// Engineer:      Eason Yeh (Yeh Hsuan-Yu)
// Create Date:   2026/04/27
// Design Name:   HybridAcc
// Module Name:   cluster_pkg
// Project Name:  HybridAcc
// Target Devices: ASIC
// Tool Versions: Synopsys VCS W-2024.09-SP1
// Description:   Cluster-level shared types and constants.
//                Mirrors hybridacc::cluster::* in Cluster/ControlTypes.hpp,
//                AddressGenerateUnit.hpp, HybridDataDeliverUnit.hpp,
//                ClusterControlUnit.hpp.
// Dependencies:  None
// Revision:
//   2026/04/27 - Initial version (M0 contract extraction)
// Additional Comments:
//   None
//-----------------------------------------------------------------------------
`ifndef CLUSTER_PKG_SV
`define CLUSTER_PKG_SV

package cluster_pkg;

    // ---------------------------------------------------------------------
    // Unified ctrl / status bit positions (ControlTypes.hpp)
    // ---------------------------------------------------------------------
    localparam int unsigned CTRL_START      = 0;
    localparam int unsigned CTRL_STOP       = 1;
    localparam int unsigned CTRL_SOFT_RESET = 2;

    localparam int unsigned STATUS_IDLE     = 0;
    localparam int unsigned STATUS_BUSY     = 1;
    localparam int unsigned STATUS_DONE     = 2;
    localparam int unsigned STATUS_QUIESCED = 3;
    localparam int unsigned STATUS_ERROR    = 4;

    typedef enum logic [1:0] {
        MODE_DIRECT_DEBUG  = 2'd0,
        MODE_LAYER_MANAGED = 2'd1
    } cluster_mode_e;

    typedef enum logic [2:0] {
        SUBSTATE_IDLE           = 3'd0,
        SUBSTATE_STARTING       = 3'd1,
        SUBSTATE_RUNNING        = 3'd2,
        SUBSTATE_STOPPING       = 3'd3,
        SUBSTATE_WAIT_QUIESCED  = 3'd4,
        SUBSTATE_SOFT_RESETTING = 3'd5,
        SUBSTATE_ERROR          = 3'd6
    } cluster_substate_e;

    typedef enum logic [1:0] {
        CLUSTER_ACTION_NONE       = 2'd0,
        CLUSTER_ACTION_NOC_START  = 2'd1,
        CLUSTER_ACTION_NOC_STOP   = 2'd2,
        CLUSTER_ACTION_NOC_RESET  = 2'd3
    } cluster_action_e;

    // Cluster MMIO base/size (ComputeCluster slave region)
    localparam logic [31:0] CLUSTER_MMIO_BASE = 32'h0000_2100;
    localparam logic [31:0] CLUSTER_MMIO_SIZE = 32'h0000_0100;

    // Cluster control register offsets (bank-local)
    localparam logic [7:0] CLUSTER_REG_MODE       = 8'h00;
    localparam logic [7:0] CLUSTER_REG_CTRL       = 8'h04;
    localparam logic [7:0] CLUSTER_REG_STATUS     = 8'h08;
    localparam logic [7:0] CLUSTER_REG_ERROR_CODE = 8'h0C;
    localparam logic [7:0] CLUSTER_REG_SUBSTATE   = 8'h10;

    // ---------------------------------------------------------------------
    // AGU MMIO offsets (bank-local, 8-bit address)
    // (AddressGenerateUnit.hpp::AguRegOffset)
    // ---------------------------------------------------------------------
    localparam logic [7:0] AGU_REG_BASE_ADDR    = 8'h00;
    localparam logic [7:0] AGU_REG_BASE_ADDR_H  = 8'h04;
    localparam logic [7:0] AGU_REG_ITER01       = 8'h08;
    localparam logic [7:0] AGU_REG_ITER23       = 8'h0C;
    localparam logic [7:0] AGU_REG_STRIDE0      = 8'h10;
    localparam logic [7:0] AGU_REG_STRIDE1      = 8'h14;
    localparam logic [7:0] AGU_REG_STRIDE2      = 8'h18;
    localparam logic [7:0] AGU_REG_STRIDE3      = 8'h1C;
    localparam logic [7:0] AGU_REG_CTRL         = 8'h20;
    localparam logic [7:0] AGU_REG_STATUS       = 8'h24;
    localparam logic [7:0] AGU_REG_LANE_CFG     = 8'h28;
    localparam logic [7:0] AGU_REG_TAG_BASE     = 8'h40;
    localparam logic [7:0] AGU_REG_TAG_STRIDE0  = 8'h44;
    localparam logic [7:0] AGU_REG_TAG_STRIDE1  = 8'h48;
    localparam logic [7:0] AGU_REG_TAG_CTRL     = 8'h4C;
    localparam logic [7:0] AGU_REG_MASK_CFG     = 8'h54;
    localparam logic [7:0] AGU_REG_ERR_CODE     = 8'h58;
    localparam logic [7:0] AGU_REG_DBG_TAG      = 8'h5C;
    localparam logic [7:0] AGU_REG_DBG_ADDR     = 8'h60;

    localparam int unsigned AGU_CTRL_ULTRA_BIT = 3;

    typedef enum logic [1:0] {
        AGU_FSM_IDLE = 2'd0,
        AGU_FSM_RUN  = 2'd1,
        AGU_FSM_DONE = 2'd2
    } agu_fsm_e;

    // ---------------------------------------------------------------------
    // HDDU plane id (HybridDataDeliverUnit.hpp::PlaneId)
    // ---------------------------------------------------------------------
    typedef enum logic [1:0] {
        PLANE_PS  = 2'd0,
        PLANE_PD  = 2'd1,
        PLANE_PLI = 2'd2,
        PLANE_PLO = 2'd3
    } plane_id_e;

    // ---------------------------------------------------------------------
    // Fixed-width Cluster datapath payloads used by current ComputeCluster
    // instantiation (ADDR_WIDTH=32, HDDU/SPM width=192).
    // ---------------------------------------------------------------------
    localparam int unsigned CLUSTER_ADDR_WIDTH = 32;
    localparam int unsigned CLUSTER_DATA_WIDTH = 192;

    typedef struct packed {
        logic [CLUSTER_ADDR_WIDTH-1:0] addr;
        logic [CLUSTER_DATA_WIDTH-1:0] wdata;
        logic                          wen;
    } spm_req_32_192_t;

    typedef struct packed {
        logic [CLUSTER_DATA_WIDTH-1:0] rdata;
        logic                          code;
    } spm_resp_192_t;

endpackage : cluster_pkg

`endif // CLUSTER_PKG_SV
