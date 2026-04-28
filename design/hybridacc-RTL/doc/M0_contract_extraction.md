# M0 — Cluster / Core / SoC RTL Contract Extraction

本文件依 implement_plan.md 中 M0 milestone，為 Cluster、Core、HybridAcc top 三層 RTL
重寫工作建立統一的「介面 / 狀態機 / 寄存器 / package」契約，後續 M1~M5 各模組實作必須與本文件對齊。
所有條目皆對應到目前 SystemC ESL 標頭，以 ESL 為 golden reference。

## 0. 全域對齊原則

- 命名沿用既有 PE/NoC 風格：模組名 PascalCase、port snake_case、reg 加 `_reg`、
  combinational 訊號加 `_sig`。
- handshake 統一採 `valid / ready`（既有 `vr_if`，定義於
  src/hybridacc_utils_pkg.sv）。
- 所有 register reset 均為 `negedge reset_n` 同步釋放 active-low。
- package 不夾帶 hierarchical 名稱；ESL 中 `hybridacc::cluster::*` 對應 RTL 的
  `cluster_pkg::*`，`hybridacc::core::*` 對應 `core_pkg::*`。
- shared SRAM primitive 直接重用 src/PE/SRAM_SP_BWEB.sv，不另開新 wrapper。

## 1. Cluster 契約

### 1.1 共用型別 (cluster_pkg)

對應 ESL `Cluster/ControlTypes.hpp`：

| 類別 | RTL 名稱 | 寬度 | 說明 |
|---|---|---|---|
| ctrl bit | `CTRL_START / CTRL_STOP / CTRL_SOFT_RESET` | 32-bit | bit0 / bit1 / bit2 |
| status bit | `STATUS_IDLE / BUSY / DONE / QUIESCED / ERROR` | 32-bit | bit0~bit4 |
| mode | `MODE_DIRECT_DEBUG / MODE_LAYER_MANAGED` | 2-bit | enum |
| substate | `SUBSTATE_IDLE / STARTING / RUNNING / STOPPING / WAIT_QUIESCED / SOFT_RESETTING / ERROR` | 3-bit | enum |
| cluster mmio | `CLUSTER_MMIO_BASE = 32'h2100` / `CLUSTER_MMIO_SIZE = 32'h0100` | const |
| cluster reg offsets | `CLUSTER_REG_MODE = 8'h00 / REG_CTRL = 8'h04 / REG_STATUS = 8'h08 / REG_ERROR_CODE = 8'h0C / REG_SUBSTATE = 8'h10` | 8-bit | |

### 1.2 AddressGenerateUnit (AGU)

ESL: `Cluster/AddressGenerateUnit.hpp`。

#### 寄存器表 (8-bit cfg_addr，bank-local)

| Offset | 名稱 | 寬度 | 功能 |
|---|---|---|---|
| 0x00 | BASE_ADDR | 32 | base 位址 (word64) |
| 0x04 | BASE_ADDR_H | 32 | reserved，目前必須寫 0 |
| 0x08 | ITER01 | 16+16 | iter0 / iter1 |
| 0x0C | ITER23 | 16+16 | iter2 / iter3 |
| 0x10 | STRIDE0 | 32 | idx0 stride |
| 0x14 | STRIDE1 | 32 | idx1 stride |
| 0x18 | STRIDE2 | 32 | idx2 stride |
| 0x1C | STRIDE3 | 32 | idx3 stride |
| 0x20 | CTRL | 32 | bit0 START / bit1 STOP / bit2 SOFT_RESET / bit3 ULTRA |
| 0x24 | STATUS | 32 | bit0 IDLE / bit1 BUSY / bit2 DONE / bit3 QUIESCED / bit4 ERROR |
| 0x28 | LANE_CFG | 32 | bit0~3 idx→addr，bit8~11 idx→tag |
| 0x40 | TAG_BASE | 32 | low 6 bit |
| 0x44 | TAG_STRIDE0 | 32 | low 8 bit |
| 0x48 | TAG_STRIDE1 | 32 | low 8 bit |
| 0x4C | TAG_CTRL | 32 | bit0 enable / bit1 idx1 src / bit2 idx0 src |
| 0x54 | MASK_CFG | 32 | low 16 bit |
| 0x58 | ERR_CODE | 32 | last gen error |
| 0x5C | DBG_TAG | 32 | last gen tag (RO) |
| 0x60 | DBG_ADDR | 32 | last gen addr (RO) |

#### Port

```text
clk, reset_n
cfg_write, cfg_addr[7:0], cfg_wdata[31:0], cfg_rdata[31:0]
start, stop                      // 與 CTRL.bit0/1 邏輯或
gen_valid, gen_ready, gen_addr[31:0], gen_tag[15:0], gen_ultra, gen_mask[15:0]
busy, done, fsm_state[1:0]       // IDLE=0, RUN=1, DONE=2
```

#### Pipeline

5 段 pipeline：`s0 -> s1 -> n0_s0 -> n0_s1 -> s2`，加上 `s1 -> s2` bypass（中間 stage 全空且 s2_ready）。
loop counter idx[0]=innermost、idx[3]=outermost；issue_new 受 `run_last_issued` 控制。
DONE state 維持 1 cycle 後回 IDLE。

### 1.3 ScratchpadMemory (SPM)

ESL: `Cluster/ScratchpadMemory.hpp`。

關鍵設計參數（對齊 ComputeCluster instantiation）：

- NUM_NOC_PORTS = 4，BANKS_PER_GROUP = 3，BANK_DATA_WIDTH = 64，BANK_DEPTH = 8192。
- TOTAL_BANKS = 12 = NUM_GROUPS(=4) * BANKS_PER_GROUP。
- NoC payload width = BANKS_PER_GROUP * BANK_DATA_WIDTH = 192-bit。
- ADDR_WIDTH = 32，MAX_OUTSTANDING = 8，DMA_MAX_OUTSTANDING = 8。

子單元拆解（建議 RTL 切成這些子模組）：

| 子模組 | 角色 |
|---|---|
| `spm_skid_buffer` | 每個 NoC port 的 1-deep skid |
| `spm_bank_arb` | NoC + DMA 仲裁，產生 per-bank req |
| `spm_group_meta_fifo` | 每個 group 的 in-flight read meta |
| `spm_resp_merge` | 把 BANKS_PER_GROUP 個 bank resp 合併成 NoC port resp |
| `spm_dma_axi` | AXI4-Lite slave -> AW/W merge -> bank fire -> B/R |
| `spm_pmu` | cycle / port_txn / arb_stall / credit_stall / DMA stat |
| `spm_top` | ScratchpadMemory port 對外整合 |

Port 對應 ESL：

- `clk / reset_n / pmu_rst_i / drop_noc_resp_i / soft_reset_i / config_map_i[7:0] / config_update_i / arb_policy_i`
- 4× `spm_req_valid_i / spm_req_ready_o / spm_req_i (struct: addr,write,strb,data,mode)`
- 4× `spm_resp_valid_o / spm_resp_ready_i / spm_resp_o (data,write_resp)`
- AXI4-Lite slave: `s_axi_aw* / s_axi_w* / s_axi_b* / s_axi_ar* / s_axi_r*` (64-bit data)
- PMU 4× port_txn[63:0] + cycle/arb_stall/credit_stall

`spm_quiesced` = 全部 FIFO empty + skid empty + DMA pending == 0。

### 1.4 HybridDataDeliverUnit (HDDU)

ESL: `Cluster/HybridDataDeliverUnit.hpp`。

- 4 plane: PS / PD / PLI = send (SPM read → NoC req)，PLO = receive (NoC resp → SPM write)。
- 包含 4 個 AGU 子實例（每個 plane 一個），bank-local MMIO base = `0x000 / 0x100 / 0x200 / 0x300`。
- Global MMIO 區段 `0x800 ~ 0x8FF`：CTRL / STATUS / PLANE_EN / PLANE_MODE / NUM_PLANES / PORT_WIDTH / ARB_POLICY / ERR_CODE / ERR_INFO0/1 / COUNTER_TX_PKT / COUNTER_TX_BYTE / COUNTER_RX_BYTE / COUNTER_STALL。
- DATA_BITS = 192（同 SPM aggregate width），NOC_TAG_BITS = 6。

子模組建議：

| 子模組 | 角色 |
|---|---|
| `hddu_send_plane` | 一個 plane：AGU+SPM read+NoC req FIFO+wait_tag FIFO |
| `hddu_recv_plane` | PLO plane：AGU+NoC addr req+NoC resp+SPM write FIFO |
| `hddu_mmio` | 全域 MMIO 解碼、CTRL/STATUS/PLANE_EN/counters |
| `hddu_top` | 整合 + interrupt 邏輯 |

### 1.5 ClusterControlUnit (CCU)

ESL: `Cluster/ClusterControlUnit.hpp`，是純邏輯 helper，RTL 中改為一個小型 FSM 模組：

- 寄存器：`mode (2b) / substate (3b) / error_code (32b) / layer_active / stop_pending / soft_reset_pending / done_sticky`
- 介面：
  - 來自 ComputeCluster: `mmio_write_mode / mmio_write_ctrl / mmio_write_err`
  - tick 輸入: `noc_quiesced / spm_quiesced`
  - tick 輸出: `noc_action[1:0] (NONE/START_PE/STOP_PE/RESET) / spm_soft_reset`
  - 直接 NoC 命令 mirror: `notify_direct_start_pe / stop_pe / reset` (single-cycle pulse)
  - 對外 status 讀回: `status_word[31:0]`、`substate[2:0]`、`error_code[31:0]`、`mode[1:0]`

### 1.6 ComputeCluster (top)

ESL: `ComputeCluster.hpp`。

- MMIO 子區段:
  - SPM: 0x0000~0x00FF（cfg_map / cfg_update / arb_policy / pmu_*）
  - HDDU: 0x1000~0x1FFF（直接傳遞給 hddu.mmio）
  - NoC: 0x2000~0x20FF（cmd_data / status）
  - Cluster: 0x2100~0x21FF（mode / ctrl / status / err / substate）
- 對外介面：native cmd frontend（valid/ready + addr/wdata/wstrb + rdata/err）+ AHB-Lite slave + 64-bit AXI4-Lite slave。
- 內部 instantiate：`spm`、`hddu`、`noc` 三個子模組。
- Power gating: `power_enable_i & reset_n` 產生 `local_reset_n_sig`。
- ClusterControlUnit 嵌在 ComputeCluster 內，AHB / native cmd 路徑都會 tap 它的 status / write API。

## 2. Core 契約

### 2.1 Core 共用型別 (core_pkg)

對應 `Core/Types.hpp`：

- AHB-Lite 寬度: ADDR=32, DATA=32, HSIZE=3, HBURST=3。
- IRQ 數量: PLIC source = 8（依 ESL 設定）。
- DMA descriptor: src_addr / dst_addr / length / flags（依 `DmaEngine.hpp`）。
- ISRAM/DSRAM size 與 base address 對齊 ESL。

### 2.2 子模組角色（細節由各檔頭再萃取）

| 模組 | 角色 |
|---|---|
| BootHostIf | host AXI4-Lite ↔ internal cmd 交握與 boot section trigger |
| CmdFabric | host cmd / firmware cmd 進入 cluster cmd path |
| CmdToAhbBridge | 把 native cmd 介面轉成 AHB-Lite master |
| ClusterDataFabric | host AXI ↔ 多個 cluster AXI slave 的 fabric |
| DmaEngine | DRAM ↔ ISRAM/DSRAM/SPM 的 DMA |
| CoreLocalIrq | local IRQ aggregation |
| Plic | platform-level interrupt controller |
| DataSram / Isram / SectionLoader | core-local memory 與 ELF section loader |
| CoreController | 上述 IP 的整合與 boot/run state 管理 |
| CoreMcu | rv32i 5 段 pipeline (Fetch/Decode/Execute/Memory/WB) |

### 2.3 rv32i pipeline (CoreMcu)

對應 `Core/rv32i_mcu/*`：

- 5 段: Fetch / Decode / Execute / Memory / WriteBack
- 共用 component: GPR (32×32)、CSR (machine mode)、ALU (rv32i 指令子集)
- shared package: PipelineTypes_pkg
- 介面: i$/d$ → ISRAM / DSRAM via AHB-Lite

詳細寄存器/control hazard 規則待 M4 進入 CoreMcu 實作前再展開。

## 3. SoC top 契約 (HybridAcc)

ESL: `HybridAcc.hpp`。

- 包含 1 個 CoreController + N 個 ComputeCluster。
- 對外: host AXI4-Lite (control)、shared DRAM AXI master、IRQ output。
- per-cluster: cluster cmd fabric + cluster data fabric + cluster IRQ。

## 4. RTL 目錄落點

```
design/hybridacc-RTL/
├─ src/
│  ├─ Cluster/
│  │  ├─ cluster_pkg.sv          (M0)
│  │  ├─ AddressGenerateUnit.sv  (M1)
│  │  ├─ ScratchpadMemory.sv     (M1, 拆子模組)
│  │  ├─ HybridDataDeliverUnit.sv(M1, 拆子模組)
│  │  ├─ ClusterControlUnit.sv   (M2)
│  │  └─ ComputeCluster.sv       (M2)
│  ├─ Core/
│  │  ├─ core_pkg.sv             (M3)
│  │  ├─ BootHostIf.sv           (M3)
│  │  ├─ CmdFabric.sv            (M3)
│  │  ├─ CmdToAhbBridge.sv       (M3)
│  │  ├─ ClusterDataFabric.sv    (M3)
│  │  ├─ DmaEngine.sv            (M3)
│  │  ├─ CoreLocalIrq.sv         (M3)
│  │  ├─ Plic.sv                 (M3)
│  │  ├─ DataSram.sv / Isram.sv / SectionLoader.sv (M3)
│  │  ├─ CoreController.sv       (M3)
│  │  ├─ CoreMcu.sv              (M4)
│  │  └─ rv32i/                  (M4)
│  └─ HybridAcc.sv               (M5)
└─ tb/
   ├─ Cluster/, Core/, SoC/      (對應 tb)
```

## 5. 開放議題

- Core/* 與 rv32i_mcu/* 的詳細 register / encoding 在 M3、M4 開工前需逐檔再萃取。
- ESL 中 `hybridacc::FIFO<T>` 的 generic param 須在 RTL 換成 packed struct（已決定使用既有 FIFO.sv）。
- SystemC 的 `sc_biguint<192>` 在 RTL 對應 `logic [191:0]`，packed struct 內以 array of 64-bit lanes 表示。
