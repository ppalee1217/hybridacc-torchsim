# HACC Core Controller RTL Hierarchy 與實作切分

本文件補充 [Core.md](Core.md) 的 RTL 落地方式，定義第二版 MCU-driven controller 的 top-level hierarchy、檔案切分與建議整合順序。若本文件與 [Core.md](Core.md) 有衝突，以 [Core.md](Core.md) 為準。

---

## 1. 建議 RTL 目錄結構

```text
rtl/
  core_controller/
    hacc_core_controller_pkg.sv
    hacc_core_controller_top.sv
    cc_boot_host_if.sv
    cc_boot_rom.sv                 # optional
    cc_section_loader.sv
    cc_mem_axi_arbiter.sv          # optional; phase-only 實作可省略
    cc_isram.sv
    cc_data_sram.sv
    cc_core_mcu.sv
    cc_cmd_fabric.sv
    cc_irq_router.sv
    cc_dma_engine.sv
    cc_cluster_data_fabric.sv
    cc_trace_pmu.sv                # optional
```

---

## 2. Top-level Hierarchy

```text
hacc_core_controller_top
  |- cc_boot_host_if
  |- cc_boot_rom                 # optional
  |- cc_section_loader
  |- cc_mem_axi_arbiter          # optional
  |- cc_isram
  |- cc_data_sram
  |- cc_core_mcu
  |- cc_cmd_fabric
  |- cc_irq_router
  |- cc_dma_engine
  |- cc_cluster_data_fabric
  |- cc_trace_pmu                # optional
```

本版 hierarchy 的核心原則是：runtime control 只經過 `cc_core_mcu`，不再插入 `cc_job_manager`、`cc_block_fetch`、`cc_block_expander`、`cc_wave_scheduler` 這類舊版主控制模組。

---

## 3. 模組責任與交付邊界

### 3.1 `hacc_core_controller_top`

責任：

1. 對外暴露 host CSR、DRAM AXI、cluster AHB/AXI 與 interrupt ports。
2. 連接 loader、local memories、MCU、DMA、IRQ router 與 external fabrics。
3. 承接 compile-time parameters。

### 3.2 `cc_boot_host_if`

責任：

1. host CSR decode
2. manifest queue
3. host-visible status / irq summary

交付邊界：

1. 不做 loader state machine
2. 不直接控制 cluster/NLU/DMA runtime path

### 3.3 `cc_boot_rom`

責任：

1. reset vector
2. boot mode 決策
3. 跳轉到 I-SRAM entry 或 host-assisted boot stub

交付邊界：

1. optional module
2. 不做 descriptor 解譯或 wave orchestration

### 3.4 `cc_section_loader`

責任：

1. 消費 manifest entry
2. 從 DRAM 取 section payload
3. 將 `.hacc.core`、`.hacc.job`、`.hacc.block`、`.hacc.profile`、`.hacc.dma`、`.hacc.agu`、`.hacc.nlu`、`.hacc.pe`、`.hacc.scan`、`.hacc.patch` 載入 local memories

交付邊界：

1. 不直接對 cluster 發 NoC command
2. 不直接對 DMA engine 發 command
3. 不直接對 NLU 發 config stream

### 3.5 `cc_mem_axi_arbiter`

責任：

1. 協調 `cc_section_loader` 與 `cc_dma_engine` 共用單一 DRAM AXI4 master
2. 在需要 runtime reload 時提供 deterministic arbitration

交付邊界：

1. 若實作採 phase ownership，可用較小實作取代完整仲裁器
2. 對外仍只允許一組 `m_mem_axi_*`

### 3.6 `cc_isram`

責任：

1. 存放 `.hacc.core`
2. 提供 MCU instruction fetch
3. 接受 loader 寫入

### 3.7 `cc_data_sram`

責任：

1. 作為 unified local data SRAM，承載 runtime descriptor、payload、trace、debug 與 completion scratch data
2. 以軟體 ABI 區分 descriptor/payload region 與 event/debug region
3. 提供 MCU 32-bit word access
4. 接受 loader 寫入

### 3.8 `cc_core_mcu`

責任：

1. 執行 HACC Core MCU ISA
2. 讀 local tables
3. 對 `cc_cmd_fabric` 發 blocking MMIO / broadcast / stream command
4. 處理 subroutine、IRQ entry/return 與 fault

交付邊界：

1. 是唯一 architecturally visible runtime control master
2. 不得被其他 module 繞過直接替它產生 hidden runtime side effect

### 3.9 `cc_cmd_fabric`

責任：

1. 解碼 MCU command
2. 路由到 local CSR、DMA MMIO、cluster AHB、NLU AHB
3. 實作 all-target-complete broadcast sequencer

交付邊界：

1. command path 維持 blocking semantics
2. 不把 broadcast 簡化成單一虛假的 AHB transfer

### 3.10 `cc_irq_router`

責任：

1. 接收 cluster / DMA / NLU level IRQ
2. 產生 sticky pending bitmap
3. 支援 explicit ack 與 reassert semantics

### 3.11 `cc_dma_engine`

責任：

1. 接受 MCU MMIO 與 stream programming
2. 對 DRAM 與 cluster SPM 做資料搬運
3. 回報 done/error irq

交付邊界：

1. 不可自行從 `cc_data_sram` 的 descriptor/payload ABI region fetch `.hacc.dma`
2. 只執行 MCU 已經下發的 runtime payload

### 3.12 `cc_cluster_data_fabric`

責任：

1. 協調 DMA engine 與 NLU data requester 對 cluster AXI4-Lite data port 的存取
2. 依 cluster id 路由 request/response

交付邊界：

1. 不應假設存在獨立 PE 或 profile AXI window
2. 主要目標是 cluster SPM data aperture

### 3.13 `cc_trace_pmu`

責任：

1. 收集 command count、stall、error、cycle 等統計
2. 提供 debug trace hook

交付邊界：

1. optional module
2. 不改變 architectural timing semantics

---

## 4. 建議 RTL 實作順序

1. 先完成 `hacc_core_controller_pkg.sv`、typedef 與 top-level port package。
2. 完成 `cc_boot_host_if`、`cc_isram`、`cc_data_sram`。
3. 完成 `cc_irq_router` 與 local CSR 定義。
4. 完成 `cc_cmd_fabric` 的 local route 與單 target AHB path。
5. 加入 broadcast sequencer。
6. 完成 `cc_section_loader`。
7. 完成 `cc_dma_engine` 與 `cc_cluster_data_fabric`。
8. 最後完成 `cc_core_mcu` 與 end-to-end bring-up。

這個順序的目的，是先讓 local load 與 command path 穩定，再把 MCU 韌體接上，降低整體 bring-up 風險。

---

## 5. Stub 使用方式

本文件對應的 `.sv` 檔案可先以 integration skeleton 形式建立，但需滿足以下條件：

1. 模組名稱與 port 方向固定
2. blocking command/response 握手固定
3. stream path 使用 `valid/ready/data/control`
4. 不得保留舊版 execution-complex modules 的空 stub，以免誤導整合與驗證

---

## 6. 審查重點

Reviewer 在看 RTL hierarchy 時，應優先確認：

1. runtime 主路徑是否真的以 `cc_core_mcu` 為中心
2. `cc_section_loader` 是否只做 local copy
3. `cc_dma_engine` 是否完全由 MCU command/stream 驅動
4. `cc_cmd_fabric` 的 broadcast completion 是否為 all-target complete
5. `cc_irq_router` 是否符合 level + sticky + ack reassert 模型
