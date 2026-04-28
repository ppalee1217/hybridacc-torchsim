# HybridAcc RTL 實作計劃書

## 1. 目的與範圍

本文件以目前的 SystemC ESL model 為 golden reference，重新整理 RTL 實作順序、可沿用基線與驗證策略。現階段 PE 與 NoC 已可視為既有可用 baseline；本輪工作的主軸不再是回收或修補舊 NoC 成果，而是因應最新版 ESL 的 Cluster 與 Core 行為更新，重新撰寫對應 RTL model 與 testbench。

本計劃涵蓋三個面向：

1. 最新 ESL 架構與 RTL 對應邊界。
2. 可沿用的 RTL baseline，以及需要重寫或持續深化的 Cluster / Core / Top 模組。
3. 以 ESL tests 與 workload 為核心的 unit test、integration test、與 regression 計劃。

本文件同時作為目前實作落地狀態的盤點紀錄。與先前版本不同，Cluster、Core、HybridAcc top 已不再是「尚未開始」，而是已有可整合的 functional / minimal baseline；但這些 baseline 仍未達成完整 ESL fidelity 與 dynamic verification closure。

### 1.1 最終驗收目標（收斂版）

後續 workload closure 不再以「泛稱的 GEMM / conv1x1 / conv3x3 regression」作為模糊目標，而是收斂到三個既有 `hybridacc-cc` example workload：

1. `design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml`
2. `design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml`
3. `design/hybridacc-cc/example/gemm/gemm_single_wave.yaml`

這三個 case 代表目前最小而具代表性的 single-wave 驗收集合。最終驗收條件應明確定義為：

- workload 來源必須來自現有 `hybridacc-cc` YAML，而不是再手寫一套獨立 RTL-only 測資。
- RTL 端必須以 VCS 執行模擬，而不是只停留在 ESL / runtime-check 的 `sim.log` 結果。
- 三個 workload 都必須通過 RTL 驗證。

需要特別說明的是：目前 repo 中 `hybridacc-cc` 的公開流程已能穩定產出 firmware / IR / runtime-check 類 artefact，但尚未看到一條直接把這三個 YAML 自動轉成 `tb_cluster_sim_advanced.sv` 所需 `rtl_cluster_case.cfg` case directory 的公開固定流程。因此目前的收斂方向應拆成兩步：

1. 先把三個 YAML 穩定接到現有 VCS 可執行的 RTL 驗證路徑。
2. 再補齊由 `hybridacc-cc` 輸出直接生成 cluster workload case directory 的橋接流程，讓 VCS workload bench 不再依賴手工或舊測資目錄。

## 2. ESL 架構基線

### 2.1 SystemC ESL 目前的系統階層

目前 ESL 已經是完整系統模型，主要階層如下：

```text
HybridAcc
├─ CoreController
│  ├─ BootHostIf
│  ├─ CmdFabric
│  ├─ CmdToAhbBridge
│  ├─ ClusterDataFabric
│  ├─ DmaEngine
│  ├─ CoreLocalIrq
│  ├─ Plic
│  ├─ DataSram / Isram / SectionLoader
│  └─ CoreMcu (rv32i pipeline)
└─ ComputeCluster x N
	├─ ClusterControlUnit
	├─ ScratchpadMemory
	├─ HybridDataDeliverUnit
	│  └─ AddressGenerateUnit x4
	└─ NetworkOnChip
		├─ NoCRouter
		├─ MBUS x NUM_PORTS
		└─ ProcessElement x (NUM_PORTS * NUM_PES_PER_PORT)
			├─ PErouter
			├─ IF_ID_Stage
			├─ EXE_M_Stage
			├─ EXE_A_Stage
			├─ VMULU / VADDU
			├─ DataMemory / InstructionMemory
			├─ SDMA / LDMA
			├─ LoopController
			├─ PsumRegFile
			└─ TransformRegFile
```

### 2.2 ESL 架構重點

- HybridAcc top 已具備 host AXI4-Lite control、shared DRAM AXI master、IRQ、以及 per-cluster control/data fabric。
- ComputeCluster 不只是資料路徑整合，還包含 native command frontend、AHB-Lite command path、SPM DMA path、cluster substate 與 run-cycle 管理。
- HDDU 是 cluster data plane 的核心橋接器，負責 4 個 AGU、SPM 四埠、NoC 的 PS/PD/PLI/PLO 四條資料面。
- NetworkOnChip 採 dual-plane / 4-channel data architecture，背後由 NoCRouter、MBUS、ProcessElement 組成。
- ProcessElement 已在 ESL 內明確拆成 IF_ID / EXE_M / EXE_A 三段 pipeline，並有對應的 local memories、DMA、router 與 register files。
- Core 子系統已超出單純 command sequencer，包含 DMA、IRQ、memory fabric、section loading，以及 rv32i MCU pipeline。

### 2.3 ESL 目前已具備的測試面

ESL test 目標已包含以下層級：

- PE：test_pe_unit、test_pe_sim
- NoC：test_noc_unit、test_noc_sim
- Cluster：test_sram_unit、test_spm_unit、test_agu_unit、test_hddu_unit、test_cluster_unit、test_cluster_control_unit、test_cluster_sim、test_cluster_sim_advanced
- Core：test_core_unit、test_core_controller_integration、test_core_sim

這代表 RTL 不應只補 module source，本質上還必須同步補齊對應的 verification ladder。RTL 目前另外多出一個 top-level smoke test，這是本輪 bring-up 的本地擴充，不代表 ESL 的完整 workload coverage 已在 RTL 落地。

### 2.4 RTL 對齊原則

- PE / NoC 以現有 RTL 實作為固定基線；除非最新版 ESL 明確更動介面或協定，否則不主動改動其外部邊界。
- Cluster / Core 必須重新從目前 ESL 類別、資料流、控制狀態與 test target 萃取 RTL spec，不沿用已移除的舊 cluster/core RTL 當主體實作來源。
- RTL testbench 命名與驗證層級以 ESL tests 為主，並維持 unit -> integration -> workload regression 的驗證階梯。
- 文件上的進度標示必須區分「已存在 baseline RTL」與「已達完整 ESL 對齊並驗證完成」，避免把 bring-up baseline 誤寫成 full completion。

## 3. 當前 RTL 現況總結

### 3.1 嚴格盤點結論

目前 workspace 的實際狀態如下：

- PE / NoC baseline 仍完整存在，且相關 RTL 與 tb 可繼續作為 regression 基底。
- Cluster RTL 已重新建立 active source tree，不再是待重寫空狀態。目前有 cluster_pkg.sv、AddressGenerateUnit.sv、ScratchpadMemory.sv、HybridDataDeliverUnit.sv、ClusterControlUnit.sv、ComputeCluster.sv。
- Core RTL 已重新建立 active source tree，不再是待重寫空狀態。目前有 core_pkg.sv、Isram.sv、DataSram.sv、CoreLocalIrq.sv、Plic.sv、BootHostIf.sv、CmdFabric.sv、CmdToAhbBridge.sv、ClusterDataFabric.sv、DmaEngine.sv、SectionLoader.sv、CoreMcu.sv、CoreController.sv。
- Top-level SoC RTL 已存在，HybridAcc.sv 已可串接 CoreController 與 ComputeCluster array。
- Cluster testbench 已有 8 個檔案；Core testbench 目前只有 2 個；top-level 目前有 1 個 smoke test。
- 本環境可使用 VCS（需透過 tcsh）執行 RTL 模擬；Cluster 端現有 unit / integration ladder 已取得動態 pass 結果。lint / synthesis flow 仍未在本輪盤點內覆蓋。

### 3.2 目前工作樹中的實際落地項目

| 區塊 | 現有 RTL source | 現有 testbench | 目前判定 |
|---|---|---|---|
| PE / NoC baseline | 既有 src/PE、src/NoC、NetworkOnChip.sv | 既有 tb/PE、tb/NoC 與 common tb | 可沿用的成熟 baseline |
| Cluster | 7 個 active source | 8 個 tb/Cluster 測試 | 已有 functional baseline，且現有 Cluster ladder 已動態驗證通過 |
| Core | 13 個 active source | 2 個 tb/Core 測試 | 已有 minimal / baseline RTL，但驗證明顯不足 |
| SoC top | HybridAcc.sv | tb_hybridacc_smoke.sv | 已有 top-level smoke baseline |

### 3.3 成熟度分級

為避免過度樂觀，現況應拆成三層：

1. 已有 active RTL 且 editor diagnostics 乾淨：Cluster、Core、HybridAcc top 皆已達到此層。
2. 仍屬 functional / minimal baseline：ScratchpadMemory、HybridDataDeliverUnit、ComputeCluster、BootHostIf、CmdFabric、CmdToAhbBridge、ClusterDataFabric、DmaEngine、Isram、DataSram、CoreLocalIrq、Plic、SectionLoader、CoreMcu、CoreController、HybridAcc。
3. 尚未達成完整 closure：Core 的細粒度 unit tb、ESL core_sim 對應驗證、SoC workload tests、lint/synthesis/gate-level checks、NLU active path。

其中 CoreMcu、SectionLoader、DmaEngine 尤其不能宣稱已與 ESL 等價：它們目前都是為了 bring-up 與整體串接而寫出的最小可用版本。

## 4. 可沿用與已建立的 baseline

| 範圍 | 內容 | 目前定位 | 計劃中的處理方式 |
|---|---|---|---|
| PE | DataMemory、Decoder、IF_ID_Stage、EXE_M_Stage、EXE_A_Stage、InstructionMemory、LDMA、LoopController、PErouter、PsumRegFile、SDMA、TransformRegFile、VADDU、VMULU、ProcessElement | 已有 active source 與既有驗證資產 | 保持介面穩定，作為 Cluster bring-up 的回歸基底 |
| NoC | MBUS、NoCRouter、NetworkOnChip | 已驗證的既有 baseline | 保留既有 regression；僅在 Cluster 串接暴露新介面落差時做最小修正 |
| Common | FIFO、async FIFO、utility package 與 common testbench 支援檔 | 可沿用的共同基礎建設 | 直接重用；若 Cluster/Core 需要新型別或 wrapper，再以相容方式擴充 |
| Cluster rewrite baseline | cluster_pkg、AGU、SPM、HDDU、CCU、ComputeCluster | 已有 active source 與 smoke-level tb | 不回到「從零開始」；以現有 baseline 為基礎補 fidelity 與補測試 |
| Core rewrite baseline | core_pkg、local SRAM、IRQ、fabric、DMA、loader、MCU、CoreController | 已有 active source，但多數模組仍是 minimal / baseline | 以現有 code 為整合基底，優先補測試與行為缺口，而不是重建空架構 |
| SoC baseline | HybridAcc.sv、tb_hybridacc_smoke.sv | 已可做 top-level smoke bring-up | 以此為 workload regression 與系統驗證的起點 |

### 4.1 對既有 baseline 的執行原則

- 不再把 PE / NoC 當成主要 recovery 項目；它們的責任是提供穩定整合環境與回歸保護。
- Cluster / Core / HybridAcc 現在也已有新的 baseline，後續工作應是補強與驗證，而不是文件上仍寫成未開始。
- 若 Cluster / Core 重寫需要改 shared package 或共用介面，必須先確認不破壞既有 PE / NoC regression。

## 5. 已完成重寫與仍待深化的 RTL 模組

### 5.1 Cluster

| 模組 | 目前狀態 | 現況說明 |
|---|---|---|
| cluster_pkg.sv | 已完成 | 已定義 command/status encoding、register offsets、shared types |
| SRAM.sv | 已完成（simulation fake model） | 已補回 ESL 對應 SRAM primitive 與 tb_sram 單元測試 |
| AddressGenerateUnit.sv | 已完成 | 已有單元測試且動態 PASS |
| ScratchpadMemory.sv | 已完成 baseline + hardmacro integration | 已改為 direct TSMC SRAM primitive-backed bank storage，保留 NoC arbitration、DMA slave path 與 PMU 欄位，現有 tb_spm / cluster integration tests 動態 PASS |
| HybridDataDeliverUnit.sv | 已完成 baseline | 已整合 4 個 AGU 與 SPM/NoC data plane，現有 tb_hddu 動態 PASS |
| ClusterControlUnit.sv | 已完成 | 已有對應 unit testbench，且已對齊 ESL pure-logic action semantics |
| ComputeCluster.sv | 已完成 baseline | 已整合 native command、AHB-lite MMIO、SPM DMA、NoC sideband；tb_computecluster_unit / tb_cluster_sim / tb_cluster_sim_advanced 已動態 PASS |

目前 Cluster 已不再缺 standalone SRAM 對應測試，且 ScratchpadMemory 也已切到 shared TSMC SRAM primitive 路徑。現有 Cluster 測試梯度已在本環境實際跑通。接下來 Cluster 端的主要缺口轉為：進一步 workload 擴展（例如 conv3x3 minimal ic-sweep）、以及更完整的 closure 工作。

### 5.2 Core

| 群組 | 模組 | 目前狀態 | 現況說明 |
|---|---|---|---|
| Shared package | core_pkg.sv | 已完成 | 已建立 address map、CSR/manifest/DMA constants 與 shared types |
| Local memory / IRQ | Isram.sv、DataSram.sv、CoreLocalIrq.sv、Plic.sv | 已完成 functional baseline | Isram 已補上 rodata data-read path，DataSram 已修正 byte-lane write alignment；timer/MMIO 路徑已被 top-level firmware case 動態驗證 |
| Host / fabric | BootHostIf.sv、CmdFabric.sv、ClusterDataFabric.sv | 已完成 functional baseline | 已能提供 host CSR、MMIO decode、cluster data routing；cluster pending-read hold 與 DMA MMIO fastpath 已補齊，現有 firmware cases 可實際打到 local/timer/dma/cluster MMIO |
| Bridge | CmdToAhbBridge.sv | 僅 source 完成 | 目前未發現被 CoreController 或 HybridAcc 實際整合使用，仍屬未落地到 system path 的模組 |
| DMA / loader | DmaEngine.sv、SectionLoader.sv | 已完成 functional baseline | SectionLoader flat 32-bit word stream 路徑已被 top-level firmware harness 實際使用；DmaEngine 已補齊 back-to-back submit from DONE/ERROR、DMA_STATUS idle semantics、以及 one-shot completion IRQ，top-level test_dma 已動態 PASS |
| MCU | CoreMcu.sv | 已完成 bring-up-oriented functional subset | 已支援目前驗證所需的 RV32I ALU/branch/jump、Zmmul（mul/mulh/mulhsu/mulhu）、DSRAM byte/halfword/word load-store、ISRAM rodata loads、word MMIO load-store、Zicsr 基本 CSR、machine ECALL trap、MEIP/MSIP/MTIP post-retire interrupt entry、mret、WFI；仍非完整 ESL rv32 pipeline fidelity |
| Core top | CoreController.sv | 已完成 functional baseline | 已整合 BootHostIf、loader、SRAM、CoreMcu、CmdFabric、Plic、DMA、ClusterDataFabric；smoke path 與目前所有 top-level control-plane firmware cases 均已動態 PASS |

Core 的結論是「已超出純 smoke baseline，但仍未達 full fidelity」。目前 CoreMcu / DmaEngine / Plic / timer 的 machine-mode control-plane firmware 已能在 top-level SoC path 上實際跑通；真正剩餘的主缺口已收斂到 workload-level regression、NLU active path、以及更完整的 closure work，而不再是 trap/interrupt/DMA/fabric 基本控制面無法運作。

### 5.3 SoC / Top

| 模組 | 目前狀態 | 現況說明 |
|---|---|---|
| HybridAcc.sv | 已完成 baseline | 已整合 CoreController 與 NUM_CLUSTERS 個 ComputeCluster |
| NLU 路徑 | 尚未啟用 | 目前 top 以 NUM_NLU=0 使用，NLU command/data/IRQ 路徑為 tie-off baseline |
| tb_hybridacc_smoke.sv | 已完成 | 已提供 top-level smoke test，動態 PASS |
| tb_hybridacc_sim.sv | 已完成（generic firmware harness） | 已支援以 plusargs 載入不同 firmware image；目前已作為 top-level firmware basic/control-plane 驗證主 harness |

## 6. Testbench 實作現況與缺口

### 6.1 目前 RTL testbench tree 的實際狀態

```text
design/hybridacc-RTL/tb/
├─ PE/                 (existing baseline)
├─ NoC/                (existing baseline)
├─ Cluster/
│  ├─ tb_agu.sv
│  ├─ tb_sram.sv
│  ├─ tb_spm.sv
│  ├─ tb_hddu.sv
│  ├─ tb_cluster_control_unit.sv
│  ├─ tb_computecluster_unit.sv
│  ├─ tb_cluster_sim.sv
│  └─ tb_cluster_sim_advanced.sv
├─ Core/
│  ├─ tb_corecontroller_smoke.sv
│  └─ tb_dma_engine.sv
├─ tb_hybridacc_smoke.sv
└─ tb_hybridacc_sim.sv
```

文件上原先規劃的 tb/SoC 目錄目前尚未建立；top smoke 與 generic firmware harness 仍在 tb 根目錄。這不影響功能，但代表 test organization 仍未收斂到原計劃格式。

### 6.2 ESL 測試目標與 RTL 對應狀態

| ESL test target | RTL 對應狀態 | 判定 |
|---|---|---|
| test_agu_unit | tb/Cluster/tb_agu.sv | 已有對應，動態 PASS |
| test_sram_unit | tb/Cluster/tb_sram.sv | 已補齊對應，動態 PASS |
| test_spm_unit | tb/Cluster/tb_spm.sv | 已有對應，動態 PASS |
| test_hddu_unit | tb/Cluster/tb_hddu.sv | 已有對應，動態 PASS |
| test_cluster_control_unit | tb/Cluster/tb_cluster_control_unit.sv | 已有對應，動態 PASS |
| test_cluster_unit | tb/Cluster/tb_computecluster_unit.sv | 近似對應，動態 PASS |
| test_cluster_sim | tb/Cluster/tb_cluster_sim.sv | 已有對應，動態 PASS |
| test_cluster_sim_advanced | tb/Cluster/tb_cluster_sim_advanced.sv | 已有對應，動態 PASS |
| test_core_unit | 無單一對應；目前僅有 tb_dma_engine.sv 與 tb_corecontroller_smoke.sv 部分覆蓋 | 部分完成 |
| test_core_controller_integration | tb/Core/tb_corecontroller_smoke.sv 可視為 smoke-level 對應 | 部分完成 |
| test_core_sim | 無直接對應 | 缺口 |
| Top-level smoke | tb_hybridacc_smoke.sv | RTL 已有本地 smoke 測試，動態 PASS |
| Top-level firmware basic / control-plane | tb/tb_hybridacc_sim.sv（generic plusarg harness） | 已驗證 PASS：test_alu、test_branch、test_jump、test_loadstore、test_mul、test_stack、test_diag、test_compound、test_hazard、test_sram_timing、test_csr、test_cluster_ctrl、test_fabric、test_dma、test_trap、test_wfi_timer、test_plic |

### 6.3 驗證成熟度判定

目前 verification status 應明確描述為：

- 已有 testbench source tree，且 Cluster 端已建立從 unit 到 integration 的基本 ladder；目前 8 個 Cluster tests 皆已在 VCS 動態 PASS。
- Core 端不再只有 smoke-level coverage：除 tb_dma_engine / tb_corecontroller_smoke 外，已透過 HybridAcc top-level firmware harness 實際驗證多個 RV32I/Zmmul/CSR/MMIO cases。
- top-level 已不只 smoke；目前 generic firmware harness 已驗證 17 個 firmware basic / control-plane cases，但仍未把 `hybridacc-cc` 的三個 single-wave example workload 穩定接進 RTL workload regression。
- 本環境已有可執行 simulator flow，且 Cluster、Core 基本控制面、trap/interrupt/WFI、DMA/PLIC workflow、以及 SoC control-plane firmware bring-up 已有動態證據；剩餘缺口已轉為 workload regression 與 closure tooling。

因此，目前可以宣稱現有 Cluster ladder 已「全綠」，而 Core / SoC 也已跨過純 smoke 階段；但 Core / SoC 仍不能宣稱達成完整 ESL closure，因為 interrupt/trap、DMA e2e、部分 fabric semantics 與 workload-level regression 尚未完成。

## 7. 分期實作計劃（依實際進度校正）

| 里程碑 | 目前狀態 | 已落地內容 | 主要缺口 |
|---|---|---|---|
| M0 ESL Contract Extraction | 已完成 | 已有 contract 文件、cluster_pkg、core_pkg、模組切分與介面基線 | 若 ESL 再變動需重新同步文件 |
| M1 Cluster Datapath Rewrite | 已完成並驗證 | SRAM、AGU、SPM、HDDU 已有 active RTL 與對應測試；ScratchpadMemory 已切到 shared TSMC SRAM primitive 路徑，且現有 unit tests 已動態 PASS | 仍待更高階 workload 驗證 |
| M2 Cluster Control + Integration | 已完成並驗證 | ClusterControlUnit、ComputeCluster、cluster smoke / advanced smoke 已落地並動態 PASS | 仍待更高階 workload 與 closure 驗證 |
| M3 Core Control Plane Rewrite | 已完成並部分驗證 | BootHostIf、CmdFabric、ClusterDataFabric、DMA、IRQ、local SRAM、CoreController 骨架已存在，且 core-issued local/timer/dma/cluster MMIO path 已被多個 firmware cases 打到 | 缺細粒度 core unit tb；CmdToAhbBridge 尚未落入 active integration path |
| M4 Core MCU + Core Integration | 大幅完成並驗證 | CoreMcu 現已能執行目前驗證所需的 RV32I/Zmmul/Zicsr 子集，並支援 DSRAM、ISRAM rodata、MMIO 控制面、machine trap/interrupt、mret、WFI；tb_corecontroller_smoke、test_trap、test_wfi_timer、test_plic 等已動態 PASS | 缺 core_sim 對應驗證與更高階 fidelity / nested-trap class 行為 |
| M5 HybridAcc Top + Workload Bring-up | 大幅完成並驗證 | HybridAcc.sv、tb_hybridacc_smoke.sv 與 generic firmware harness 已存在；目前 17 個 top-level firmware control-plane cases 動態 PASS | 缺把 `conv2d_1x1_single_wave`、`conv2d_3x3_single_wave`、`gemm_single_wave` 三個 `hybridacc-cc` workload 接進 VCS 驗證；NLU path 尚未啟用 |
| M6 Closure | 未開始 | 僅完成 editor diagnostics 等靜態檢查 | 尚無 lint、synthesis、gate-level checks |

## 8. 後續執行順序（由現況出發）

1. 以目前已綠的 control-plane firmware harness 為基礎，先收斂三個明確 workload 驗收 case：`conv2d_1x1_single_wave`、`conv2d_3x3_single_wave`、`gemm_single_wave`；其來源固定為現有 `hybridacc-cc` example YAML，並要求最終以 VCS 跑過 RTL。
2. 補齊剩餘 verification gap：Core 細粒度 unit tb、SoC workload tests，以及 Cluster 端更高階 workload / closure 驗證。
3. 補一條由 `hybridacc-cc` 輸出自動生成 VCS workload stimulus 的固定橋接流程；若短期內無法直接生出 `rtl_cluster_case.cfg` case directory，至少要先把 compiler 產生的 firmware artefact 穩定接到現有 top-level VCS harness。
4. 決定 CmdToAhbBridge 的定位：要嘛真正接入 CoreController / cluster path，要嘛自 active milestone 移除，避免文件與實作分離。
5. 決定 NLU path 的啟用順序與驗證策略；目前 top 仍以 NUM_NLU=0 tie-off baseline 運作。
6. 補齊 lint、synthesis、selected gate-level checks，使 closure 不只停留在 editor diagnostics 與 RTL sim。

## 9. 風險與阻塞點

### 9.1 當前主要阻塞

- Cluster 端已有完整動態 PASS 證據；Core / SoC 端的 trap/interrupt/WFI/DMA/fabric control-plane firmware 也已有 top-level 動態 PASS 證據。
- Core 驗證面雖已明顯前進，但細粒度 unit tb 仍少；若不補 unit tb，剩餘 bug 仍會集中在 CoreController integration 與 top-level firmware bring-up 才爆出。
- 多個關鍵模組明確標示為 minimal / baseline，若文件不區分成熟度，會誤導後續開發與驗證規劃。
- CmdToAhbBridge 尚未進入 active integration path，NLU path 仍為 tie-off，代表架構上仍有未完成分支。

### 9.2 技術風險

- CoreMcu 雖已支援目前所需的 machine trap/interrupt/WFI 語意，但仍是 bring-up-oriented execution core；若未來需要更完整的 privilege / nested-trap fidelity，仍可能暴露 corner case。
- SectionLoader 與 DmaEngine 已足以支撐目前 control-plane firmware 與 DMA loopback / transform cases；但一旦進入更高流量 workload，仍可能暴露 timing / buffering / overlap corner case。
- ComputeCluster 目前採 functional baseline 與近似 quiesce 判定，若後續需要更精確的 lifecycle / overlap 行為，可能必須回頭補控制邏輯。
- 若 ESL 再次變動 Cluster/Core contract，而文件沒有持續以實際 codebase 校正，計劃書會再次失真。

## 10. Definition of Done 與目前差距

| 條件 | 目前狀態 |
|---|---|
| PE / NoC baseline 維持可用並作為 Cluster / Core bring-up 基底 | 已滿足 |
| Cluster / Core / SoC 目標模組都有 active RTL source | 已滿足，但多數僅達 baseline / minimal 水準 |
| Cluster / Core / SoC 各層都有對應 unit test 與 integration test | 部分滿足；Cluster 較完整，Core 與 SoC 明顯不足 |
| `conv2d_1x1_single_wave`、`conv2d_3x3_single_wave`、`gemm_single_wave` 可由現有 `hybridacc-cc` example 生成，並在 RTL/VCS 路徑下通過驗證 | 未滿足 |
| 主要模組具備 lint、synthesis、selected gate-level checks 的可追蹤結果 | 未滿足 |

因此，本輪工作目前最準確的定性是：Cluster bring-up 與現有 Cluster verification ladder 已完成並取得動態 PASS；Core / SoC 的 control-plane firmware bring-up 也已大幅完成，trap/interrupt/WFI/DMA/fabric 類 top-level firmware cases 已動態 PASS。後續主戰場不再是抽象的 workload closure，而是三個明確的 `hybridacc-cc` single-wave example 要能走通 RTL/VCS 驗證，再向外擴展到更高階 workload、NLU path 與實體設計前檢查。

## 11. 盤點依據

本文件的盤點依據來自下列現有資產：

- ESL 架構與 top-level：design/hybridacc-ESL/simulator/include/HybridAcc.hpp、ComputeCluster.hpp、Core.hpp
- ESL 測試清單：design/hybridacc-ESL/test/CMakeLists.txt
- 現行 RTL baseline：design/hybridacc-RTL/src/PE、design/hybridacc-RTL/src/NoC、design/hybridacc-RTL/src/NetworkOnChip.sv、design/hybridacc-RTL/src/FIFO.sv、design/hybridacc-RTL/src/asyncFIFO.sv、design/hybridacc-RTL/src/hybridacc_utils_pkg.sv
- 本輪已落地的 Cluster source：design/hybridacc-RTL/src/Cluster
- 本輪已落地的 Core source：design/hybridacc-RTL/src/Core
- 本輪已落地的 top-level RTL：design/hybridacc-RTL/src/HybridAcc.sv
- 現行 RTL testbench baseline：design/hybridacc-RTL/tb/PE、design/hybridacc-RTL/tb/NoC、design/hybridacc-RTL/tb/tb_common.svh、design/hybridacc-RTL/tb/tb_fifo.sv、design/hybridacc-RTL/tb/tb_asyncfifo.sv、design/hybridacc-RTL/tb/tb_networkonchip.sv
- 本輪已落地的 Cluster / Core / top-level testbench：design/hybridacc-RTL/tb/Cluster、design/hybridacc-RTL/tb/Core、design/hybridacc-RTL/tb/tb_hybridacc_smoke.sv
- workload stimulus：testbench/pe、testbench/noc、testbench/cluster、testbench/core
- 本次盤點的 workspace 實際檔案清查與 editor diagnostics 靜態檢查結果


# 注意事項
- 本主機只有 vcs 環境，需要在 tcsh 中才能使用，因為環境變數設定位於 .tcshrc 中。