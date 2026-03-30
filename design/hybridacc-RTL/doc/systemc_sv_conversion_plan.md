# HybridAcc SystemC -> SystemVerilog 轉換計畫

## 目標

本文件整理以下工作範圍的現況、缺口與建議實作順序。

- 來源 SystemC 模組目錄: /home/easonyeh/hybridacc/design/hybridacc-ESL/simulator
- 來源 SystemC 測試目錄: /home/easonyeh/hybridacc/design/hybridacc-ESL/test
- 目標 SystemVerilog 模組目錄: /home/easonyeh/hybridacc/design/hybridacc-RTL/src
- 目標 SystemVerilog 測試平台目錄: /home/easonyeh/hybridacc/design/hybridacc-RTL/tb

## 盤點結論

### 1. 目標目錄現況

- /home/easonyeh/hybridacc/design/hybridacc-RTL/src 目前為空。
- /home/easonyeh/hybridacc/design/hybridacc-RTL/tb 目前為空。
- 倉庫根目錄已存在一批可用 RTL 與單元測試，可作為第一批匯入來源:
  - /home/easonyeh/hybridacc/src
  - /home/easonyeh/hybridacc/tb

### 2. ESL 可辨識模組群

依照 simulator/include 盤點，SystemC 設計可分成四層:

- 基礎資料通道與 NoC 元件
  - FIFO
  - asyncFIFO
  - MBUS
  - NoCRouter
  - NetworkOnChip
- PE 資料路徑與子模組
  - InstructionMemory
  - Decoder
  - IF_ID_Stage
  - EXE_A_Stage
  - EXE_M_Stage
  - DataMemory
  - LDMA
  - SDMA
  - LoopController
  - PsumRegFile
  - TransformRegFile
  - VADDU
  - VMULU
  - PErouter
  - ProcessElement
- Cluster 級資料通路
  - AddressGenerateUnit
  - HybridDataDeliverUnit
  - SRAM
  - ScratchpadMemory
  - ComputeCluster
- Core / SoC 周邊模組
  - BootHostIf
  - CmdFabric
  - ClusterDataFabric
  - ComputeClusterBusAdapter
  - CoreController
  - CoreMcu
  - DataSram
  - DmaEngine
  - IrqRouter
  - Isram
  - SectionLoader

補充:

- HybridAcc.hpp 目前是空檔。
- Core.hpp 目前只是一組 include 聚合，不是 SC_MODULE。
- utils.hpp 內的 VRDIF / VRDOF / VRDSIG 是驗證/介面輔助結構，不建議視為獨立可綜合 RTL 模組。

## RTL 覆蓋矩陣

下表中的「既有 RTL」與「既有 TB」指的是倉庫根目錄下已存在的 /src 與 /tb，不是 design/hybridacc-RTL 目標目錄。

| 模組 | ESL 來源 | 既有 RTL | 既有 TB | 狀態 |
| --- | --- | --- | --- | --- |
| FIFO | simulator/include/FIFO.hpp | src/FIFO.sv | tb/tb_fifo.sv | 可直接匯入目標目錄 |
| asyncFIFO | simulator/include/async_FIFO.hpp | src/asyncFIFO.sv | tb/tb_asyncfifo.sv | 可直接匯入目標目錄 |
| MBUS | simulator/include/NoC/MBUS.hpp | src/NoC/MBUS.sv | tb/NoC/tb_mbus.sv | 可直接匯入目標目錄 |
| NoCRouter | simulator/include/NoC/NoCRouter.hpp | src/NoC/NoCRouter.sv | tb/NoC/tb_nocrouter.sv | 可直接匯入目標目錄 |
| NetworkOnChip | simulator/include/NetworkOnChip.hpp | src/NetworkOnChip.sv | tb/tb_networkonchip.sv, tb/NoC/tb_noc_unit_rtl.sv, tb/NoC/tb_noc_sim_rtl.sv | 可直接匯入，需補模組頂部註釋 |
| InstructionMemory | simulator/include/PE/InstructionMemory.hpp | src/PE/InstructionMemory.sv | tb/PE/tb_instructionmemory.sv | 可直接匯入目標目錄 |
| Decoder | simulator/include/PE/Decoder.hpp | src/PE/Decoder.sv | tb/PE/tb_decoder.sv | 可直接匯入目標目錄 |
| IF_ID_Stage | simulator/include/PE/IF_ID_stage.hpp | src/PE/IF_ID_Stage.sv | tb/PE/tb_if_id_stage.sv | 可直接匯入目標目錄 |
| EXE_A_Stage | simulator/include/PE/EXE_A_stage.hpp | src/PE/EXE_A_Stage.sv | tb/PE/tb_exe_a_stage.sv | 可直接匯入目標目錄 |
| EXE_M_Stage | simulator/include/PE/EXE_M_stage.hpp | src/PE/EXE_M_Stage.sv | tb/PE/tb_exe_m_stage.sv | 可直接匯入目標目錄 |
| DataMemory | simulator/include/PE/DataMemory.hpp | src/PE/DataMemory.sv | tb/PE/tb_datamemory.sv | 可直接匯入目標目錄 |
| LDMA | simulator/include/PE/LDMA.hpp | src/PE/LDMA.sv | tb/PE/tb_ldma.sv | 可直接匯入目標目錄 |
| SDMA | simulator/include/PE/SDMA.hpp | src/PE/SDMA.sv | tb/PE/tb_sdma.sv | 可直接匯入目標目錄 |
| LoopController | simulator/include/PE/LoopController.hpp | src/PE/LoopController.sv | tb/PE/tb_loopcontroller.sv | 可直接匯入目標目錄 |
| PsumRegFile | simulator/include/PE/PsumRegFile.hpp | src/PE/PsumRegFile.sv | tb/PE/tb_psumregfile.sv | 可直接匯入目標目錄 |
| TransformRegFile | simulator/include/PE/TransformRegFile.hpp | src/PE/TransformRegFile.sv | tb/PE/tb_transformregfile.sv | 可直接匯入目標目錄 |
| VADDU | simulator/include/PE/VADDU.hpp | src/PE/VADDU.sv | tb/PE/tb_vaddu.sv | 可直接匯入目標目錄 |
| VMULU | simulator/include/PE/VMULU.hpp | src/PE/VMULU.sv | tb/PE/tb_vmulu.sv | 可直接匯入目標目錄 |
| PErouter | simulator/include/PE/PErouter.hpp | src/PE/PErouter.sv | tb/PE/tb_perouter.sv | 可直接匯入目標目錄 |
| ProcessElement | simulator/include/ProcessElement.hpp | src/PE/ProcessElement.sv | tb/PE/tb_processelement.sv | 可直接匯入目標目錄 |
| AddressGenerateUnit | simulator/include/Cluster/AddressGenerateUnit.hpp | src/Cluster/AddressGenerateUnit.sv | tb/Cluster/tb_addressgenerateunit.sv | ✅ 已完成 RTL 與單元測試 |
| HybridDataDeliverUnit | simulator/include/Cluster/HybridDataDeliverUnit.hpp | src/Cluster/HybridDataDeliverUnit.sv | tb/Cluster/tb_hddu.sv | ✅ 已完成 RTL 與單元測試 |
| SRAM | simulator/include/Cluster/SRAM.hpp | src/Cluster/SRAM.sv | tb/Cluster/tb_sram.sv | ✅ 已完成 RTL 與單元測試 |
| ScratchpadMemory | simulator/include/Cluster/ScratchpadMemory.hpp | src/Cluster/ScratchpadMemory.sv | tb/Cluster/tb_scratchpadmemory.sv | ✅ 已完成 RTL 與單元測試 |
| ComputeCluster | simulator/include/ComputeCluster.hpp | src/Cluster/ComputeCluster.sv | tb/Cluster/tb_computecluster.sv | ✅ 已完成 RTL 與整合測試 |
| BootHostIf | simulator/include/Core/BootHostIf.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| CmdFabric | simulator/include/Core/CmdFabric.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| ClusterDataFabric | simulator/include/Core/ClusterDataFabric.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| ComputeClusterBusAdapter | simulator/include/Core/ComputeClusterBusAdapter.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| CoreController | simulator/include/Core/CoreController.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| CoreMcu | simulator/include/Core/CoreMcu.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| DataSram | simulator/include/Core/DataSram.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| DmaEngine | simulator/include/Core/DmaEngine.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| IrqRouter | simulator/include/Core/IrqRouter.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| Isram | simulator/include/Core/Isram.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |
| SectionLoader | simulator/include/Core/SectionLoader.hpp | 無 | 無 | 必須新寫 RTL 與單元測試 |

## 三個指定整合測試的轉換需求

### test_pe_sim.cpp

角色:

- 單一 PE 的卷積模擬測試。
- 載入指令、權重、activation、partial sum。
- 驗證輸出資料與容忍誤差。

主要輸入格式:

- pe_program.bin
- weight.bin
- activation_input.bin
- ps_input.bin
- activation_output.bin
- meta.txt

SV 轉寫需求:

- 需要 binary file loader。
- 需要 meta.txt parser。
- 需要可重用的 PE driver tasks。
- 需要 golden comparison 與誤差容忍設定。

### test_noc_sim.cpp

角色:

- NoC 層級功能與資料搬運測試。
- 驗證 scan-chain、command broadcast、資料路由與輸出。

主要輸入格式:

- config.txt
- scan_chain.bin
- input_activation.bin
- input_weight.bin
- input_partial_sum.bin
- output_partial_sum.bin
- pe_program.bin

SV 轉寫需求:

- 需要 config.txt parser。
- 需要 scan-chain 封包產生器。
- 需要 NoC 命令封包與資料通道 driver。
- 需要能重用於 Cluster test 的 transaction library。

### test_cluster_sim_advanced.cpp

角色:

- Cluster 級整合測試。
- 涵蓋 SPM、HDDU、NoC、DMA、AHB/AXI-lite 控制。
- 支援 conv2d、gemm、both 三種 case。

主要輸入格式:

- JSON 風格設定檔或等效結構化 case 設定。
- activation / weight / partial_sum / output 等二進位檔。
- AGU 與 DMA 相關配置資料。

SV 轉寫需求:

- 需要 test case parser。
- 需要 AXI-lite 與 AHB-lite bus functional model。
- 需要 AGU / DMA 配置 tasks。
- 需要 timeout / trace / verbose 控制。
- 需要沿用與 C++ 測試相同的檔案格式，不可任意改資料目錄佈局。

## Makefile 擴充方向

目前 /home/easonyeh/hybridacc/design/hybridacc-RTL/Makefile 只處理綜合，不處理 testbench compile/run。

建議拆成三類目標:

### 1. 單元測試前模擬

- sim_<module>
- sim_all
- sim_pe
- sim_noc
- sim_cluster

建議行為:

- 以 tb 對應檔案編譯執行。
- 產生 compile log 與 run log。
- 支援傳入 TEST_DATA_DIR、TRACE、VERBOSE、CLOCK_NS、TIMEOUT_CYCLES。

### 2. 綜合

- synthesize_<module>
- synall

建議行為:

- 保留現有流程。
- 將 synthesis script 命名統一為 synthesis_<module>.tcl。
- 補上對子目錄模組的搜尋規則，不只掃描 src/*.sv。

### 3. 模擬後檢查

- postsim_<module>
- postsim_all

建議行為:

- 比對 golden output。
- 彙整 error summary。
- 失敗時保留波形與 log 路徑。

## 分階段實作建議

### Phase 0: 建立目標目錄基線

- 將倉庫根目錄已有 RTL 與 TB 匯入 design/hybridacc-RTL/src 與 design/hybridacc-RTL/tb。
- 所有匯入模組檔案頂部補上註釋，至少包含:
  - 模組名稱
  - 功能摘要
  - 主要參數
  - 主要 I/O 群組

完成標準:

- 目標目錄不再為空。
- 既有 20 個 RTL 模組與 23 個 TB 可從 design/hybridacc-RTL 獨立編譯。

### Phase 1: 補齊 Cluster 基礎模組 ✅ 已完成

優先順序:

1. AddressGenerateUnit ✅
2. SRAM ✅
3. ScratchpadMemory ✅
4. HybridDataDeliverUnit ✅

原因:

- 這四者是 ComputeCluster 與 cluster-level testbench 的必要基底。
- 其中 AGU 已經有 synthesis_agu.tcl，表示既有 flow 已預留此模組。

完成標準:

- 每個模組有可綜合 RTL。
- 每個模組有對應單元測試平台。
- 邊界條件至少覆蓋 reset、empty/full、越界位址、無效命令、backpressure。

### Phase 2: 補齊 Core / SoC 周邊模組

優先順序:

1. DmaEngine
2. ComputeClusterBusAdapter
3. CmdFabric
4. ClusterDataFabric
5. DataSram
6. Isram
7. CoreController
8. IrqRouter
9. BootHostIf
10. SectionLoader
11. CoreMcu

完成標準:

- 可支援 ComputeCluster 的 bus-level 控制與資料搬移。
- 能被 cluster-level testbench 直接驅動。

### Phase 3: 建立 ComputeCluster RTL ✅ 已完成

內容:

- 完成 SPM、HDDU、NoC、AHB-lite、AXI4-lite glue logic。
- 對齊 ESL 的可設定參數與外部 I/O。
- 建立 cluster 級 smoke test 與功能測試。
- ComputeCluster.sv 已通過 VCS 編譯與 10 項 assertion 測試。

完成標準:

- 可接受與 ESL 相同的測試資料目錄。
- 可跑基本 conv2d case。

### Phase 4: 轉寫三個大型整合測試

優先順序:

1. test_pe_sim.cpp -> tb/PE/tb_pe_sim.sv
2. test_noc_sim.cpp -> tb/NoC/tb_noc_sim.sv
3. test_cluster_sim_advanced.cpp -> tb/cluster/tb_cluster_sim_advanced.sv

建議共用驗證基礎設施:

- tb/common/tb_fileio_pkg.sv
- tb/common/tb_cli_pkg.sv
- tb/common/tb_compare_pkg.sv
- tb/common/tb_trace_pkg.sv
- tb/common/tb_bus_bfm_pkg.sv

完成標準:

- 支援與 C++ 測試相同的 testbench file format。
- 同一份資料目錄可直接餵給 SV testbench。
- 能輸出與 C++ 測試同級的 pass/fail summary。

## 風險與決策點

### 1. 這不是單純語法轉換

ESL 的 SystemC 測試依賴:

- 動態容器
- 檔案解析
- 浮點比較
- command line option parsing
- case 組裝與摘要輸出

SV testbench 需要額外驗證基礎設施，不能只把 C++ 逐行改寫成 initial block。

### 2. 目標目錄與既有 RTL 分離

目前可用 RTL/TB 在倉庫根目錄，不在 design/hybridacc-RTL。

建議:

- 先匯入再演進，不要在兩套 RTL 間長期維護分叉。

### 3. Core / Cluster 模組數量大

若要求一次性全部實作，風險是:

- 無法在單次工作中完成可靠驗證。
- 測試平台先於 RTL 穩定，會造成大量假性失敗。

因此建議依照 Phase 0 -> 4 漸進完成。

## 建議下一步

建議立刻執行以下順序:

1. 先把既有 RTL/TB 匯入 design/hybridacc-RTL 目標目錄。
2. 補齊所有已存在模組的檔頭註釋。
3. 更新 Makefile，先支援既有 20 個模組的模擬前與綜合流程。
4. 從 AddressGenerateUnit 開始補齊缺失模組與單元測試。
5. 最後再做三個大型整合 testbench 的資料格式相容轉寫。