# Firmware Profiling Note

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

## TL;DR

- 目前 ESL model 的 wave gap 分析主體在 simulator live probe，不是在 firmware 結束後讀一份 firmware 自寫的 profiling summary。
- wave gap 的 source of truth 是 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L71-L90](../../simulator/include/Core/CoreController.hpp#L71-L90) 的 WaveGapInstructionStats，實際探針執行在 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1234](../../simulator/include/Core/CoreController.hpp#L1234) 附近的 wave_gap_probe_thread。
- simulator 只是在 [design/hybridacc-ESL/simulator/src/main.cpp#L779-L856](../../simulator/src/main.cpp#L779-L856) 把統計列印成 [SIM] log；Python 端再由 [python/hybridacc_cc/sweep_tools.py#L74-L115](../../../../python/hybridacc_cc/sweep_tools.py#L74-L115) 與 [python/hybridacc_tools/wave_gap_summary.py#L13-L33](../../../../python/hybridacc_tools/wave_gap_summary.py#L13-L33) 解析。
- 這次靜態檢查沒有找到目前 tree 內仍存在的 firmware-side profiling ABI，例如 DSRAM 前 0x200 bytes 的 .profile summary、對應 magic/version、或 [SIM] FW profile 類型的輸出。repo memory 內的 Phase 0 profiling note 與目前 source tree 有落差，暫時應視為歷史筆記，不能直接當現況。

## 1. ESL model 目前如何分析 wave gap

### 1.1 統計資料從哪裡來

- top-level HybridAcc 對外暴露四類和 profiling 直接相關的 live counter / probe accessor：
  - cluster busy cycles: [design/hybridacc-ESL/simulator/include/HybridAcc.hpp#L438-L443](../../simulator/include/HybridAcc.hpp#L438-L443)
  - DMA active cycles: [design/hybridacc-ESL/simulator/include/HybridAcc.hpp#L445](../../simulator/include/HybridAcc.hpp#L445)
  - compute/DMA overlap cycles: [design/hybridacc-ESL/simulator/include/HybridAcc.hpp#L447-L452](../../simulator/include/HybridAcc.hpp#L447-L452)
  - wave gap stats: [design/hybridacc-ESL/simulator/include/HybridAcc.hpp#L454-L456](../../simulator/include/HybridAcc.hpp#L454-L456)

- wave gap 統計結構定義在 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L71-L90](../../simulator/include/Core/CoreController.hpp#L71-L90)。

- 重要欄位分成三類：
  - 視窗總量統計：completed_windows、dropped_partial_windows、total_cycles、total_instructions
  - gap 內 instruction bucket：total_mmio_config_instructions、total_data_compute_instructions、total_control_instructions
  - lifecycle 切段：boot_up_time、drain_out_time、以及每個 window 的 detail list

### 1.2 wave gap 視窗怎麼切

- wave gap probe thread 是一個獨立的 SystemC thread，由 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L691](../../simulator/include/Core/CoreController.hpp#L691) 註冊，主體在 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1234-L1319](../../simulator/include/Core/CoreController.hpp#L1234-L1319)。

- cluster START/STOP 不是靠 trace 後處理推測，而是直接 decode MCU 的 MMIO request：
  - decode entry: [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1040-L1059](../../simulator/include/Core/CoreController.hpp#L1040-L1059)
  - 只接受 write request
  - 只看 cluster unicast 或 broadcast 位址窗
  - 只在 local offset == 0x1800 時當成 HDDU global control
  - bit0 視為 START，bit1 視為 STOP

- 這代表目前 wave gap 的語意是：
  - 視窗起點 = 一個退休的 HDDU STOP control instruction
  - 視窗終點 = 下一個退休的 HDDU START control instruction
  - 視窗內同時計入 STOP 與 START 本身，所以 control bucket 不是零成本邊界，而是包含控制指令本體

- 直接負責切窗與累加的 helper 在：
  - start window: [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1135-L1158](../../simulator/include/Core/CoreController.hpp#L1135-L1158)
  - finish window: [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1160-L1207](../../simulator/include/Core/CoreController.hpp#L1160-L1207)
  - instruction bucket accounting: [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1120-L1133](../../simulator/include/Core/CoreController.hpp#L1120-L1133)

### 1.3 boot-up / drain-out / partial windows

- boot-up 由 core enable 到第一個 cluster START，helper 在 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1209-L1218](../../simulator/include/Core/CoreController.hpp#L1209-L1218)。

- drain-out 由最後一個 cluster STOP 到 MCU halt，helper 在 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1220-L1232](../../simulator/include/Core/CoreController.hpp#L1220-L1232)。

- dropped_partial_windows 的兩個主要來源：
  - 尚未 finish 前又收到新的 STOP
  - 模擬結束時視窗還開著，但沒有等到後續 START

### 1.4 simulator 輸出哪些 profiling log

- log producer 在 [design/hybridacc-ESL/simulator/src/main.cpp#L779-L856](../../simulator/src/main.cpp#L779-L856)。

- main.cpp 會輸出：
  - Cluster RUN cycles
  - DMA active cycles
  - Compute/DMA overlap cycles 與兩種 overlap ratio
  - wave gap 的 windows / cycles / instructions / bucket breakdown
  - boot-up 與 drain-out 的 cycles / instructions / detail
  - 每個 window 的 stop_cycle、next_start_cycle、instructions、mmio_config、data_compute、start_stop_control

- 關鍵是 main.cpp 並不重新計算 wave gap，它只是把 HybridAcc / CoreController 已經累積好的 probe state 印出來。

## 2. Python 端如何消費這些 profiling

### 2.1 主 parser: sweep_tools.py

- metric label 與 profiling 欄位定義在 [python/hybridacc_cc/sweep_tools.py#L19-L72](../../../../python/hybridacc_cc/sweep_tools.py#L19-L72)。

- sim.log regex pattern 定義在：
  - scalar metrics: [python/hybridacc_cc/sweep_tools.py#L74-L104](../../../../python/hybridacc_cc/sweep_tools.py#L74-L104)
  - boot-up detail: [python/hybridacc_cc/sweep_tools.py#L106-L109](../../../../python/hybridacc_cc/sweep_tools.py#L106-L109)
  - drain-out detail: [python/hybridacc_cc/sweep_tools.py#L110-L113](../../../../python/hybridacc_cc/sweep_tools.py#L110-L113)
  - per-window detail: [python/hybridacc_cc/sweep_tools.py#L114-L117](../../../../python/hybridacc_cc/sweep_tools.py#L114-L117)

- 實際 parsing 在 [python/hybridacc_cc/sweep_tools.py#L700-L782](../../../../python/hybridacc_cc/sweep_tools.py#L700-L782)。

- 派生指標在 [python/hybridacc_cc/sweep_tools.py#L922-L974](../../../../python/hybridacc_cc/sweep_tools.py#L922-L974)，其中最重要的是：
  - core_probe_cycles_total = drain_out_end_cycle，若沒有 detail 才退回 ebreak_cycle
  - steady_state_core_cycles = core_probe_cycles_total - boot_up_cycles - drain_out_cycles
  - wave_gap_cycles_pct_of_steady_state = wave_gap_cycles_total / steady_state_core_cycles
  - ideal_hw_sw_codesign_core_cycles = steady_state_core_cycles - wave_gap_data_compute_instructions_total

- sweep_tools 也把這組 profiling 視為 report 的正式一部分，不只是中介數據：
  - timeline 說明文字: [python/hybridacc_cc/sweep_tools.py#L1366-L1368](../../../../python/hybridacc_cc/sweep_tools.py#L1366-L1368)
  - report 說明文字: [python/hybridacc_cc/sweep_tools.py#L1637](../../../../python/hybridacc_cc/sweep_tools.py#L1637)

### 2.2 次要 summarizer: hacc-wave-gap-summary

- `uv run hacc-wave-gap-summary` 直接面向 sim.log 聚合多個 result dir；實作在 [python/hybridacc_tools/wave_gap_summary.py](../../../../python/hybridacc_tools/wave_gap_summary.py)。

- pattern 定義在 [python/hybridacc_tools/wave_gap_summary.py#L13-L33](../../../../python/hybridacc_tools/wave_gap_summary.py#L13-L33)。

- 它在說明段落中明確寫死目前 wave gap 的定義：
  - [python/hybridacc_tools/wave_gap_summary.py#L224](../../../../python/hybridacc_tools/wave_gap_summary.py#L224)
  - boot-up / drain-out 語意說明在 [python/hybridacc_tools/wave_gap_summary.py#L230-L231](../../../../python/hybridacc_tools/wave_gap_summary.py#L230-L231)

- 這代表目前 user-facing 的 wave gap 語意，其實已經同時存在於 simulator 實作與 Python 說明文字兩側；若之後你改動定義，這兩邊都要一起同步。

## 3. Firmware side 的靜態 finding

### 3.1 目前 tree 內沒有明顯的 firmware 自寫 profiling summary

- firmware template 只看到 CSR 常數定義：
  - [python/hybridacc_cc/templates/firmware_hw.h.j2#L223](../../../../python/hybridacc_cc/templates/firmware_hw.h.j2#L223)
  - [python/hybridacc_cc/templates/firmware_hw.h.j2#L224](../../../../python/hybridacc_cc/templates/firmware_hw.h.j2#L224)

- 這次靜態搜尋沒有找到：
  - CSR_MCYCLE / CSR_MINSTRET 的實際 read usage
  - 專門的 profile struct / profile header / profile magic / profile version
  - DSRAM 前 0x200 bytes 的專用 profiling ABI
  - .profile section 或 [SIM] FW profile 類型的 simulator dump

- 換句話說，現況比較像是：
  - firmware 透過正常 MMIO sequencing 驅動 accelerator
  - simulator 從這些 live event 與 retired instruction stream 做 profiling
  - Python 再從 sim.log 做二次整理

### 3.2 main.cpp 目前讀 DSRAM 的地方是 firmware test summary，不是 performance profiling

- 目前在 main.cpp 中明確讀 DSRAM word 的已知位置是 [design/hybridacc-ESL/simulator/src/main.cpp#L893-L896](../../simulator/src/main.cpp#L893-L896)。

- 這四個 word 對應 fw_check 的 test result counter: total / pass / fail / sentinel，屬於 firmware regression verdict，不是 performance / wave gap profiling。

### 3.3 與 repo memory 的衝突

- /memories/repo/runtime-profiling-phase0.md 目前寫的是：generated firmware 會把 compact summary 寫到 DSRAM 前 0x200 bytes，simulator 會自動印 [SIM] FW profile ...。

- 這次 source-based 靜態 review 沒有找到對應實作，因此目前較合理的判讀是：
  - 這份 memory 是舊版本行為或 phase note
  - 或者實作曾存在於當時的 worktree / generated build artifact，但不在目前 source tree

- 在你真的開始改 firmware profiling 前，建議先把這個 memory 視為 historical hint，而不是 current contract。

## 4. 如果接下來要修改 firmware，哪些地方最容易影響 profiling

### 4.1 會直接改變 wave gap 視窗定義的修改

- 任何改到 HDDU STOP / START MMIO 位址或 bit 定義的修改，都要同步改 [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp#L1040-L1059](../../simulator/include/Core/CoreController.hpp#L1040-L1059)，不然 simulator 會誤判 window 邊界。

- 如果把原本一個 wave 中的 STOP / START 合併、延後、提早，wave_gap_windows、last window、partial windows dropped 都可能改變。

### 4.2 只會改 bucket 分布，但不一定改 window 邊界的修改

- 把 cluster restart 前的 setup hoist 成更多 MMIO write：
  - 主要增加 mmio_config bucket

- 把更多純 firmware 計算放在 STOP -> START 之間：
  - 主要增加 data_compute bucket

- 變更 STOP / START 本身發送方式：
  - 主要改 control bucket

### 4.3 會改 lifecycle，但不一定改 wave gap 的修改

- 把初始化工作往第一個 START 之前移：
  - 主要影響 boot-up

- 把尾段 cleanup / writeback / polling 往最後一個 STOP 之後移：
  - 主要影響 drain-out

- 改 DMA overlap 讓 cluster busy 與 DMA active 關係不同：
  - 主要影響 overlap 與 util 報表
  - 只有當 STOP -> START 時距也被改變時，才會同步改 wave gap

## 5. 建議的修改落點順序

如果你下一步要改的是「wave gap 定義」或「firmware profiling 指標」，建議從下列順序切：

1. simulator probe definition
  - [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp](../../simulator/include/Core/CoreController.hpp)

2. simulator log surface
  - [design/hybridacc-ESL/simulator/src/main.cpp](../../simulator/src/main.cpp)

3. Python parser / derived metrics
  - [python/hybridacc_cc/sweep_tools.py](../../../../python/hybridacc_cc/sweep_tools.py)
  - [python/hybridacc_tools/wave_gap_summary.py](../../../../python/hybridacc_tools/wave_gap_summary.py)

4. firmware runtime sequencing
  - [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2)
  - [python/hybridacc_cc/templates/firmware_main.c.j2](../../../../python/hybridacc_cc/templates/firmware_main.c.j2)
  - [python/hybridacc_cc/templates/firmware_hw.h.j2](../../../../python/hybridacc_cc/templates/firmware_hw.h.j2)

## 6. 本次 review 的邊界

- 這份 note 是 static review，沒有重跑 simulator 或重新產生 firmware。
- 我有比對 source tree、Python consumer 與 repo memory，但沒有做 git history archaeology。
- 因此對 DSRAM 0x200-byte profile summary 的結論是「目前 source tree 未找到」，不是「歷史上從未存在」。