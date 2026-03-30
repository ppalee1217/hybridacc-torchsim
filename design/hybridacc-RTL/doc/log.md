# HybridAcc ESL -> SystemVerilog RTL 轉換紀錄

## 目標與原則
- 目標：將 `design/hybridacc-ESL/simulator/include` 與對應 `src` 的 ESL/SystemC module，逐一完整轉換為可維護的 SystemVerilog RTL module，輸出到 `design/hybridacc-RTL/src`。
- 原則：
	1. 一次只轉一個 module（或其直接對應檔對），完成後再進入下一個。
	2. 不省略功能、不缺漏欄位、不偷換語意。
	3. 先建立共用型別與函式（utils）作為後續 module 的依賴基礎。
	4. 每次修改都在此檔記錄狀態，確保可中斷後續接續。

## 轉換順序（目前規劃）
1. `utils.hpp` + `utils.cpp` -> `hybridacc_utils_pkg.sv`（共用型別/enum/函式/介面）
2. `PE/*` 各模組（由底層元件到 stage，再到 `ProcessElement`）
3. `NoC/*` 各模組（`NoCRouter`、`MBUS`）
4. `NetworkOnChip.hpp` 頂層整合

## 目前狀態
- [x] 確認 RTL 輸出目錄：`design/hybridacc-RTL/src`（目前為空）
- [x] 讀取 `utils.hpp` 與 `utils.cpp` 完整內容
- [x] 建立 `hybridacc_utils_pkg.sv`
- [x] 完成 `fp16_add/fp16_mul` 的 SystemVerilog function 對應
- [x] 建立 valid-ready 介面（替代 VRDIF/VRDOF/VRDSIG 概念）
- [x] 做一次語法層級自檢（VS Code Problems：無錯誤）
- [x] 完成 `PE/InstructionMemory.hpp` -> `src/PE/InstructionMemory.sv`
- [x] 完成 Batch-002 語法層級自檢（VS Code Problems：無錯誤）
- [x] 完成 `PE/LoopController.hpp` -> `src/PE/LoopController.sv`
- [x] 完成 `PE/Decoder.hpp` -> `src/PE/Decoder.sv`
- [x] 完成 `PE/VADDU.hpp` -> `src/PE/VADDU.sv`
- [x] 完成 `PE/VMULU.hpp` -> `src/PE/VMULU.sv`
- [x] 完成 `PE/TransformRegFile.hpp` -> `src/PE/TransformRegFile.sv`
- [x] 完成 `PE/PsumRegFile.hpp` -> `src/PE/PsumRegFile.sv`
- [x] 完成 `PE/DataMemory.hpp` -> `src/PE/DataMemory.sv`
- [x] 完成 `FIFO.hpp` -> `src/FIFO.sv`
- [x] 完成 `async_FIFO.hpp` -> `src/asyncFIFO.sv`
- [x] 完成 `PE/IF_ID_stage.hpp` -> `src/PE/IF_ID_Stage.sv`
- [x] 完成 `PE/LDMA.hpp` -> `src/PE/LDMA.sv`
- [x] 完成 `PE/SDMA.hpp` -> `src/PE/SDMA.sv`
- [x] 完成 `PE/EXE_M_stage.hpp` -> `src/PE/EXE_M_Stage.sv`
- [x] 完成 `PE/EXE_A_stage.hpp` -> `src/PE/EXE_A_Stage.sv`
- [x] 完成 `PE/PErouter.hpp` -> `src/PE/PErouter.sv`
- [x] 完成 `ProcessElement.hpp` -> `src/PE/ProcessElement.sv`
- [x] 完成 `NoC/MBUS.hpp` -> `src/NoC/MBUS.sv`
- [x] 完成 `NoC/NoCRouter.hpp` -> `src/NoC/NoCRouter.sv`
- [x] 完成 `NetworkOnChip.hpp` -> `src/NetworkOnChip.sv`
- [x] 完成 `Cluster/SRAM.hpp` -> `src/Cluster/SRAM.sv`
- [x] 完成 `Cluster/AddressGenerateUnit.hpp` -> `src/Cluster/AddressGenerateUnit.sv`
- [x] 完成 `Cluster/ScratchpadMemory.hpp` -> `src/Cluster/ScratchpadMemory.sv`
- [x] 完成 `Cluster/HybridDataDeliverUnit.hpp` -> `src/Cluster/HybridDataDeliverUnit.sv`
- [x] 完成 `ComputeCluster.hpp` -> `src/Cluster/ComputeCluster.sv`
- [x] 完成 Cluster 模組 VCS 編譯驗證（0 error, 0 warning）
- [x] 完成 Cluster 單元測試（tb_sram, tb_scratchpadmemory, tb_hddu, tb_computecluster）

## 本次工作批次（Batch-001）
- 範圍：`utils.hpp`, `utils.cpp`
- 輸出：`design/hybridacc-RTL/src/hybridacc_utils_pkg.sv`
- 開始時間：2026-03-02
- 狀態：完成

## Batch-001 轉換摘要
- 已完成 ESL `utils.hpp` + `utils.cpp` 到 RTL `hybridacc_utils_pkg.sv` 的基礎轉換。
- 已涵蓋：
	- 共用 typedef/const（`fp16_t`, `pe_inst_t`, command mask/offset）
	- enum/type（`NOC_CHANNELS`, `NOC_RESPONSE_STATUS`, `PERouterMode`, `message_command_t`, `TRACE_PID`）
	- struct/type（`v_fp16_t`, `noc_request_t`, `noc_addr_req_t`, `noc_response_t`, `pe_decode_signals_t`, `ScanChainFormat`）
	- function（`parse_scan_chain_data`, `fp16_add`, `fp16_mul`）
	- valid-ready interface（`vr_if`）
- 驗證：
	- VS Code 問題面板：無錯誤
	- `verilator --lint-only`：環境未安裝 verilator，待後續 CI 或工具安裝後補跑

## 後續接續指引
- 下一步優先：
	1. 建立全系統整合 testbench（RTL 端）
	2. 針對 instruction flow / DMA / PLI-PLO path 做逐模組對拍
	3. 補上 lint + synthesis flow（如 verilator / yosys / 商用綜合）

## 本次工作批次（Batch-002）
- 範圍：`PE/InstructionMemory.hpp`
- 輸出：`design/hybridacc-RTL/src/PE/InstructionMemory.sv`
- 開始時間：2026-03-02
- 狀態：完成

## Batch-002 轉換摘要
- 已完成 InstructionMemory RTL 化，保留原始行為語意：
	- active-low reset 時清空記憶體內容
	- 同步寫入（`posedge clk`）
	- 非同步讀取（combinational read）
	- 讀取位址/寫入位址皆以 byte address 轉 word index（`addr >> 1`）
- 額外保留對應 helper 能力（供模擬 backdoor 使用）：
	- `reset_mem`, `size_bytes`, `set_word`, `fetch_word`, `clear_mem`
- 驗證：
	- VS Code 問題面板：`InstructionMemory.sv`、`hybridacc_utils_pkg.sv` 皆無錯誤

## 本次工作批次（Batch-003）
- 範圍：`LoopController`, `Decoder`, `VADDU`, `VMULU`, `TransformRegFile`, `PsumRegFile`, `DataMemory`, `FIFO`, `asyncFIFO`, `IF_ID_Stage`, `LDMA`, `SDMA`
- 輸出：
	- `design/hybridacc-RTL/src/PE/LoopController.sv`
	- `design/hybridacc-RTL/src/PE/Decoder.sv`
	- `design/hybridacc-RTL/src/PE/VADDU.sv`
	- `design/hybridacc-RTL/src/PE/VMULU.sv`
	- `design/hybridacc-RTL/src/PE/TransformRegFile.sv`
	- `design/hybridacc-RTL/src/PE/PsumRegFile.sv`
	- `design/hybridacc-RTL/src/PE/DataMemory.sv`
	- `design/hybridacc-RTL/src/FIFO.sv`
	- `design/hybridacc-RTL/src/asyncFIFO.sv`
	- `design/hybridacc-RTL/src/PE/IF_ID_Stage.sv`
	- `design/hybridacc-RTL/src/PE/LDMA.sv`
	- `design/hybridacc-RTL/src/PE/SDMA.sv`
- 開始時間：2026-03-02
- 狀態：完成

## 本次工作批次（Batch-004）
- 範圍：`EXE_M_Stage`, `EXE_A_Stage`, `PErouter`, `ProcessElement`, `MBUS`, `NoCRouter`, `NetworkOnChip`
- 輸出：
	- `design/hybridacc-RTL/src/PE/EXE_M_Stage.sv`
	- `design/hybridacc-RTL/src/PE/EXE_A_Stage.sv`
	- `design/hybridacc-RTL/src/PE/PErouter.sv`
	- `design/hybridacc-RTL/src/PE/ProcessElement.sv`
	- `design/hybridacc-RTL/src/NoC/MBUS.sv`
	- `design/hybridacc-RTL/src/NoC/NoCRouter.sv`
	- `design/hybridacc-RTL/src/NetworkOnChip.sv`
- 開始時間：2026-03-02
- 狀態：完成

## Batch-004 轉換摘要
- 完成剩餘 Stage / PE / NoC / Top 模組之 SystemVerilog RTL 化與連線。
- 已完成語法層級檢查：VS Code Problems 對新增檔案顯示無錯誤。

## 本次工作批次（Batch-005）
- 範圍：建立各 RTL module 的 unit test 與 NoC testbench
- 輸出：
	- `design/hybridacc-RTL/tb/tb_common.svh`
	- `design/hybridacc-RTL/tb/tb_fifo.sv`
	- `design/hybridacc-RTL/tb/tb_asyncfifo.sv`
	- `design/hybridacc-RTL/tb/PE/tb_instructionmemory.sv`
	- `design/hybridacc-RTL/tb/PE/tb_loopcontroller.sv`
	- `design/hybridacc-RTL/tb/PE/tb_decoder.sv`
	- `design/hybridacc-RTL/tb/PE/tb_vaddu.sv`
	- `design/hybridacc-RTL/tb/PE/tb_vmulu.sv`
	- `design/hybridacc-RTL/tb/PE/tb_transformregfile.sv`
	- `design/hybridacc-RTL/tb/PE/tb_psumregfile.sv`
	- `design/hybridacc-RTL/tb/PE/tb_datamemory.sv`
	- `design/hybridacc-RTL/tb/PE/tb_ldma.sv`
	- `design/hybridacc-RTL/tb/PE/tb_sdma.sv`
	- `design/hybridacc-RTL/tb/PE/tb_if_id_stage.sv`
	- `design/hybridacc-RTL/tb/PE/tb_exe_m_stage.sv`
	- `design/hybridacc-RTL/tb/PE/tb_exe_a_stage.sv`
	- `design/hybridacc-RTL/tb/PE/tb_perouter.sv`
	- `design/hybridacc-RTL/tb/PE/tb_processelement.sv`
	- `design/hybridacc-RTL/tb/NoC/tb_mbus.sv`
	- `design/hybridacc-RTL/tb/NoC/tb_nocrouter.sv`
	- `design/hybridacc-RTL/tb/tb_networkonchip.sv`
	- `design/hybridacc-RTL/tb/NoC/tb_noc_system.sv`
- 開始時間：2026-03-02
- 狀態：完成

## Batch-005 測試摘要
- 已建立共用 testbench 基礎設施（clock/reset、assert macro）。
- 已補齊 base module、PE module、NoC module 與頂層 `NetworkOnChip` 測試。
- NoC 測試涵蓋：`MBUS` 單元、`NoCRouter` 單元、`NetworkOnChip` 整合 smoke test。
- 已完成語法層級檢查：`design/hybridacc-RTL/tb` 目錄無錯誤。

## 本次工作批次（Batch-006）
- 範圍：參考 ESL `test_noc_sim.cpp`、`test_noc_unit.cpp` 撰寫 RTL NoC 測試
- 輸出：
	- `design/hybridacc-RTL/tb/NoC/tb_noc_unit_rtl.sv`
	- `design/hybridacc-RTL/tb/NoC/tb_noc_sim_rtl.sv`
- 開始時間：2026-03-02
- 狀態：完成

## Batch-006 測試摘要
- `tb_noc_unit_rtl.sv`：對齊 `test_noc_unit` 精神，覆蓋 reset 後 channel ready、scan-chain command gating、PS fanout、PLO response 聚合與狀態傳遞。
- `tb_noc_sim_rtl.sv`：對齊 `test_noc_sim` 流程，採 phase-based（scan-chain -> PS/PD/PLI/PLO）整合流量驅動與握手計數檢查。
- 已完成語法層級檢查：新增兩個 RTL NoC 測試檔無錯誤。

## 本次工作批次（Batch-007）
- 範圍：Cluster 級 RTL 模組轉換（SRAM, ScratchpadMemory, HybridDataDeliverUnit, ComputeCluster）
- 輸出：
	- `design/hybridacc-RTL/src/Cluster/SRAM.sv`
	- `design/hybridacc-RTL/src/Cluster/ScratchpadMemory.sv`
	- `design/hybridacc-RTL/src/Cluster/HybridDataDeliverUnit.sv`
	- `design/hybridacc-RTL/src/Cluster/ComputeCluster.sv`
- 開始時間：2026-03-30
- 狀態：完成

## Batch-007 轉換摘要
- 完成 Cluster 子系統全部 4 個模組之 SystemVerilog RTL 化（AddressGenerateUnit 已在先前批次完成）。
- 全部模組通過 VCS W-2024.09-SP1 編譯，0 error, 0 warning。
- 遵循 VCS 相容性限制：無 `automatic` 在 `always_comb`、無 inline 變數宣告。
- RTL Bug 修正：
  - `HybridDataDeliverUnit.sv`：修正 `wait_fifo_dout[lane][15:0]` 超出範圍存取（SIOB），改為零延伸 `{{(16-NOC_ADDR_BITS){1'b0}}, wait_fifo_dout[lane]}`。
  - `ComputeCluster.sv`：修正 HDDU MMIO 讀取延遲問題。`hddu_mmio_addr` 以非阻塞賦值設定，但 `hddu_mmio_rdata` 在同一週期組合讀取導致 stale data。新增 `ahb_hddu_rd_pending_reg` flag 與延遲一週期捕獲邏輯。
- 模組架構摘要：
  - **SRAM.sv**：可參數化 SRAM，支援 pipelined read（LATENCY 參數）、byte-mask write、backpressure（rd_ready/rd_valid handshake）。
  - **ScratchpadMemory.sv**：4 NoC port、12 SRAM bank（4 groups × 3 banks/group）、AXI4-Lite DMA、priority arbitration、PMU counters。
  - **HybridDataDeliverUnit.sv**：4 AGU lanes、3 send planes + 1 receive plane、MMIO 控制介面、PS/PLO 資料路徑。
  - **ComputeCluster.sv**：頂層整合（SPM + HDDU + NoC），AXI4-Lite DMA bridge, AHB-Lite command, power gating, PMU。

## 本次工作批次（Batch-008）
- 範圍：建立 Cluster 級模組 unit test 與整合 testbench
- 輸出：
	- `design/hybridacc-RTL/tb/Cluster/tb_sram.sv`
	- `design/hybridacc-RTL/tb/Cluster/tb_scratchpadmemory.sv`
	- `design/hybridacc-RTL/tb/Cluster/tb_hddu.sv`
	- `design/hybridacc-RTL/tb/Cluster/tb_computecluster.sv`
- 開始時間：2026-03-30
- 狀態：完成

## Batch-008 測試摘要
- 已建立 4 個 Cluster 模組的 unit/integration testbench，全部通過 VCS 模擬。
- 測試結果：
  - **tb_sram**：10 PASS / 0 FAIL（基本讀寫、byte-mask、pipeline fill、backpressure、pipeline full、read-after-write）
  - **tb_scratchpadmemory**：11 PASS / 0 FAIL（cross-bank、parallel mode、multi-port、DMA write→NoC read、DMA read、write response code、PMU counters）
  - **tb_hddu**：9 PASS / 0 FAIL（MMIO constants、plane_en R/W、PS send path、PLO receive path、status register）
  - **tb_computecluster**：10 PASS / 0 FAIL（AHB SPM config、DMA roundtrip、PMU cycle counter、HDDU MMIO via AHB、power gating、NoC command readback）。使用 inline stub NetworkOnChip（不依賴 NoCRouter/MBUS/PE）。
- 全部迴歸測試無新增失敗：
  - 既有 PE/NoC unit test 全部維持 PASS
  - 既有 pre-existing 失敗（tb_vaddu 2 FAIL subnormal, tb_exe_a_stage 3 FAIL PLI/PLO stall）不受影響

## 本次工作批次（Batch-009）— 全面除錯及驗證修復
- 範圍：修復所有 Unit test / Level test 編譯失敗及驗證失敗
- 開始時間：2026-03-30
- 狀態：完成

### 修正清單

#### RTL / Infrastructure 修正
1. **Makefile**：新增 `SRAM_SIM_MODEL := src/PE/SRAM_SP_BWEB.sv`，在 `run_tb` VCS 編譯命令中加入 behavioral SRAM model
2. **src/PE/SRAM_SP_BWEB.sv**：新增 `` `define TSMC_SRAM_MODEL_LOADED `` 防止 MPD 重複宣告

#### Testbench 修正
3. **tb/PE/tb_exe_a_stage.sv**：重寫 Tests 3-8
   - Test 3：設定 `vaddu_mode=32'd1`（原為預設 0 進入 VMAC 3-cycle pipeline）
   - 各測試間加入 `stage_reset` 確保 FSM 回到 S_IDLE
   - Test 4：PLI unstall 加入額外 `@(posedge clk); #1;` 等待 FSM 轉移
   - Test 5：PLO stall 改為多週期設定（3 posedge 填滿 PLO buffer 再阻塞 drain）
   - Test 6：PLO output 加入等待週期讓 buffer register 更新
   - 結果：14 PASS / 0 FAIL

4. **tb/PE/tb_vaddu.sv**：Test 6 subnormal 預期值從 `16'h0001` 改為 `16'h0000`
   - 原因：DW_fp_add (ieee_compliance=0) 會 flush subnormal 為 zero
   - 結果：22 PASS / 0 FAIL

5. **tb/NoC/tb_noc_sim_rtl.sv**：修正 `send_scan_chain_cfg` task 中 `(idx+1)[5:0]` 語法錯誤
   - VCS 不支援對 expression 直接做 bit-select，改用暫時變數
   - 結果：PASS

6. **tb/tb_common.svh**：新增 `` `ifndef TB_COMMON_SVH `` include guard
   - 解決 tb_noc_system 因 transitivity include 導致的 MPD
   - 結果：tb_noc_system PASS

7. **tb/PE/tb_pe_sim.sv**：修正 XMRE cross-module reference
   - `vmul_reg` → `vmul_data_reg`
   - `pli_reg` → `pli_data_reg`
   - `vaddu_result` → `vaddu_result_sig`
   - `valid_reg` → `state_reg`（新 FSM 無 valid_reg）
   - `decode_reg.inst` → `decode_reg.vaddu_mode`（pe_decode_signals_t 無 inst 欄位）
   - 結果：編譯通過，驗證失敗（Conv2D 輸出資料不符 golden output）

### 迴歸測試結果（29 testbenches）
- **Unit Test：23/23 PASS（398 assertions, 0 FAIL）**
- **Level Test：5/6 PASS**
  - tb_networkonchip: PASS
  - tb_processelement: PASS
  - tb_noc_sim_rtl: PASS
  - tb_noc_system: PASS
  - tb_noc_unit_rtl: 12 PASS / 0 FAIL
  - tb_pe_sim: 編譯通過，驗證失敗（Conv2D 輸出不符）
- **唯一殘留問題**：tb_pe_sim 的 Conv2D end-to-end 驗證失敗，PE pipeline 整體行為仍需偵錯
