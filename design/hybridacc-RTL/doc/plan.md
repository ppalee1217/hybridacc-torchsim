# HybridAcc RTL SRAM / 記憶體收斂改善計劃

日期：2026-04-30

狀態：合成前審查草案

範圍：本文件定義在下一輪合成之前，針對以下三項議題所需完成的整體工作：

- 將 PE/SRAM_SP_BWEB.sv 更名並集中為 src/utils/SRAM_Wrapper.sv
- 刪除 Cluster/SRAM.sv
- 將 Core 與 Cluster 的本地記憶體行為收斂到真實 TS1N16ADFPCLLLVTA128X64M4SWSHOD macro 契約

本文件僅為規劃文件，不代表 RTL 已經完成修改。

---

## 1. 目的

目前的首要目標不是增加功能，而是先讓現有 RTL 成為一份在邏輯上自洽、在合成上合理、在模擬上可重現的設計描述。

目前有三個問題必須一起處理：

1. build flow 與 source path 仍然殘留已刪除或已搬移的 SRAM model 參考。
2. 部分 Core local-memory 模組仍使用 shadow byte array 與同拍 combinational readback，這與實際 hard macro timing 契約不相容。
3. 目前 RTL 的 Inst-RAM / CoreMcu 契約已經偏離 ESL 架構與文件定義。

如果只修其中一項，結果仍然會是脆弱的：

- 只修 path，不會解決錯誤的 timing 行為。
- 只修 memory timing，會直接打破目前 CoreMcu 的 zero-wait 假設。
- 只修 CoreMcu，不清理 build flow，simulation 與 synthesis 入口仍然彼此不一致。

---

## 2. 執行摘要

### 2.1 建議的 canonical 架構方向

建議的收斂方向如下：

1. 將 src/utils/SRAM_Wrapper.sv 視為 TS1N16ADFPCLLLVTA128X64M4SWSHOD 唯一共用的 pre-sim behavioral model。
2. 讓 Inst-RAM 回到 fetch-only，與 ESL 規格及 compiler/linker 契約一致。
3. 從 Core Isram 與 DataSram 移除 fake mem[] array，讓 macro Q 成為唯一真實的讀取資料來源。
4. 更新 CoreMcu 與 CoreController，使 local memory access 服從真實的 1-cycle memory response 契約。
5. ScratchpadMemory 維持現有 macro-backed 架構，但要明確稽核其 read-response timing 與 arbitration semantics。

### 2.2 為何建議走這個方向

這個方向可以同時對齊目前彼此不一致的四件事：

1. ESL 架構文件。
2. hybridacc-cc linker / ELF layout 文件。
3. 真實 SRAM macro timing。
4. synthesis 與 gate-sim 的預期行為。

此外，這也能移除目前 RTL Isram 中一項沒有必要的面積與維護成本：現有設計為了保留 data-side IRAM read，相當於複製了一份 hard macro storage。

---

## 3. 目前已確認的現況

本節整理目前在 workspace 中已經確認的狀態。

### 3.1 Build 與 path 現況

新的 shared wrapper 檔案目前已存在於：

- design/hybridacc-RTL/src/utils/SRAM_Wrapper.sv

但目前 build 與 synthesis 入口尚未完整收斂到這個新位置。

#### 3.1.1 Makefile 目前預設使用 foundry SRAM model，而不是 wrapper-first pre-sim

目前 RTL Makefile 會在預設 VCS flags 中定義 TSMC_SRAM_MODEL_LOADED，並直接載入 foundry SRAM Verilog model：

- design/hybridacc-RTL/Makefile

這代表：

1. 新的 shared wrapper 目前不是實際的預設 pre-sim source of truth。
2. wrapper 只有在某些 flow 剛好手動 include 時才會真正生效。
3. 所謂「pre-sim behavior model」目前仍只是文件概念，尚未變成明確的 build policy。

#### 3.1.2 RTL include directories 尚未包含 src/utils

目前 Makefile 中的 RTL_INCDIRS 僅包含：

- tb
- src
- src/Cluster
- src/Core
- src/PE
- src/NoC

尚未明確加入 src/utils。

#### 3.1.3 Top-level synthesis source patterns 尚未包含 src/utils

目前 top-level synthesis script 掃描的路徑為：

- src/*.sv
- src/Core/*.sv
- src/Cluster/*.sv
- src/NoC/*.sv
- src/PE/*.sv

但未包含 src/utils/*.sv。

這表示 top-level synthesis 可能會漏掉 shared wrapper source，除非剛好靠其他環境設定或 file ordering 補上。

#### 3.1.4 Cluster synthesis script 仍引用已刪除的 Cluster/SRAM.sv

目前 cluster synthesis script 仍直接把已刪除的 Cluster/SRAM.sv 列為下列模組的依賴：

- SRAM
- ScratchpadMemory 的 dependency
- ComputeCluster 的 dependency

這些引用已經過時，在路徑收斂後會變成明確失敗點。

#### 3.1.5 Testbench 與 helper script 仍引用舊的 PE/SRAM_SP_BWEB.sv

舊 wrapper path 目前仍存在於多個位置，包括：

- tb/tb_hybridacc_smoke.sv
- tb/tb_hybridacc_sim.sv
- tb/Cluster/cluster_rtl_stack.svh
- tb/PE/tb_pe_sim.sv
- script/add_gate_sim_support.py

csrc、sim log、build log 與歷史 report 之類的生成物中也仍可見舊路徑，但這些不應手動修補，而應在 source 收斂後重新 clean 並 regenerate。

### 3.2 Core Isram 目前狀態

目前 RTL Isram 具有以下特性：

1. 具有 instruction fetch 介面。
2. 同時也具有 data-side 介面：
   - mcu_dm_valid_i
   - mcu_dm_addr_i
   - mcu_dm_rdata_o
3. 內部含有 shadow byte array：
   - mem[0:SRAM_BYTES-1]
4. 內部實例化了兩組獨立 macro array：
   - u_if_sram array
   - u_dm_sram array
5. loader write 會同時 broadcast 到兩組 macro array。
6. instruction fetch 與 data-side read 都是由 shadow mem[] 以 combinational 方式組出讀值。

目前造成的結果：

1. 真正的功能 source of truth 是 shadow array，而不是 macro Q。
2. 資料等效上在 request address 當拍就可見。
3. 設計並未反映預期的 1-cycle SRAM read response。
4. 模組面積模型具有誤導性，因為它為了保留暫時性的相容行為而複製了 storage。

### 3.3 Core DataSram 目前狀態

目前 RTL DataSram 具有以下特性：

1. 對外提供 32-bit MCU load/store 介面。
2. 內部含有 shadow byte array：
   - mem[0:SRAM_BYTES-1]
3. 內部實例化真實 TS1N16 macro array。
4. store 同時更新 macro control 與 shadow mem[] array。
5. read 則是由 mem[] 以 combinational 方式組出回傳值。

目前造成的結果：

1. macro path 雖然存在，但 Q output 不是架構上的正式資料來源。
2. CoreMcu 角度看到的是 same-cycle、zero-wait read behavior。
3. 這與真實 hard macro timing 以及 gate-level 預期都不相容。

### 3.4 ScratchpadMemory 目前狀態

原先對 ScratchpadMemory 的懷疑需要更精準地重新定義。

目前觀察到：

1. ScratchpadMemory 內未發現 fake mem[] shadow array。
2. ScratchpadMemory 已直接實例化 TS1N16 macro。
3. read data 來源是 sram_q。
4. 設計使用 noc_read_pending_reg 與 dma_read_pending_reg 將 request issue 與 response formation 分開。

因此 ScratchpadMemory 的真正問題不是「移除 fake array」，而是：

1. 確認 request timing 是否真的符合 1-cycle macro response。
2. 確認 NOC read、parallel read、DMA read 都是使用正確的前一拍 context。
3. 確認 response backpressure 與 arbitration 在 path cleanup 後仍然正確。

### 3.5 RTL CoreMcu 目前狀態

目前 RTL CoreMcu 仍然是一個對 local memory 採 zero-wait 假設的 functional core。

已觀察到的行為：

1. 它將 instruction fetch data if_rdata_i 視為當前 cycle 可立即使用。
2. 它將 local load data ls_resp_rdata_i 視為當前 cycle 可立即使用。
3. 它會對 DSRAM 與 ISRAM 的 data-side load 都驅動 ls_req_valid_o。
4. 它沒有明確的 local-memory response-valid input。

目前造成的結果：

1. 若將 Isram/DataSram 修正成真實 1-cycle response semantics，而 CoreMcu 不改，功能就會壞掉。
2. 因此 memory retiming 不只是 wrapper 問題，而是 CoreMcu 契約問題。

### 3.6 ESL 與文件現況

ESL 的公開契約與文件在一個關鍵點上比目前 RTL 更一致。

目前已確認：

1. ESL 文件將 Inst-RAM 定義為 instruction fetch only。
2. ESL Isram 介面只有 fetch + loader write，沒有 data-side IRAM port。
3. ESL 文件將 Inst-RAM 與 Data-RAM 的 load delay 都定義為 1 cycle。
4. hybridacc-cc linker / ELF layout 將 .text 放在 ISRAM，將 .rodata/.data/.bss 放在 DSRAM。

需要補充的細節是：

ESL 的可執行實作內部仍保留一些 shortcut，例如 direct memory callback 或 read_byte 這類 helper，用來避開 SystemC delta-cycle artifact 或簡化 functional model；但就 architected integration point 而言，ESL 仍比目前 RTL CoreMcu 更接近 1-cycle registered memory contract。

結論是：

RTL 應該對齊 ESL 的架構契約，而不是對齊 ESL 內部為了模擬方便保留的 shortcut。

---

## 4. Canonical 架構決策

這是合成前最核心的設計決策。

### 4.1 選項 A：Inst-RAM 改回 fetch-only，作為正式架構方向（建議）

#### 定義

1. Isram 只支援 instruction fetch + loader write。
2. 任何 data-side load/store 進入 Inst-RAM address space 都視為架構上非法。
3. CoreController 不再將 local load/store traffic 路由到 Isram。
4. CoreMcu 不再將 Inst-RAM 視為合法的 local load target。

#### 優點

1. 與 ESL 公開介面及 Core.md 規格一致。
2. 與 hybridacc-cc 中 .rodata 放在 DSRAM 的 linker 契約一致。
3. 可移除 Isram 內重複的 macro array。
4. timing 收斂更單純。
5. 可移除不代表實際硬體意圖的面積假象。

#### 風險

1. 任何仍假設 data-side IRAM read 合法的 firmware 或 test 都會失敗。
2. 既有 tb_isram 與部分 RTL 文件需要重寫。

#### 風險評估

依目前 compiler/linker 文件判斷，此方向是合理且值得採用的。既然 software contract 已假設 .rodata 在 DSRAM，則 fetch-only Inst-RAM 反而是更乾淨的長期方向。

### 4.2 選項 B：暫時保留 data-side Inst-RAM compatibility mode（不建議作為 canonical 路徑）

#### 定義

1. 暫時保留 data-side IRAM load。
2. 仍然移除 fake mem[] 並收斂到 macro-timed behavior。
3. 將此路徑明確標記為有限期的相容模式。

#### 優點

1. 短期內對既有測試的衝擊較小。

#### 代價

1. 會迫使設計保留 duplicated storage，或加入額外 arbitration/port-sharing 邏輯。
2. 會延續一個與 ESL 架構文件衝突的契約。
3. 會增加合成判讀與長期維護成本。

#### 建議

不應將此作為預設架構。只有在確認存在無法同時移除的真實軟體依賴時，才考慮作為過渡方案。

### 4.3 實作前必須先凍結的決策

在任何 RTL 修改開始之前，專案需要正式決定以下二選一：

1. Inst-RAM data-side access 一律以 deterministic halt 處理，視為 unsupported local access。
2. Inst-RAM data-side access 一律以 architected load/store access fault 處理。

對目前偏 bring-up 導向的 RTL 而言，deterministic halt 是較低風險的短期方案；access fault 架構上更乾淨，但會擴大 trap-handling 的工作範圍。

---

## 5. 目標終態

只有在以下條件全部成立時，這次設計收斂才可視為完成。

### 5.1 Source 與 build 收斂

1. 已刪除的 Cluster/SRAM.sv 不再被任何 active source 或 script 參考。
2. 舊的 PE/SRAM_SP_BWEB.sv 不再被任何 active source 或 script 參考。
3. src/utils/SRAM_Wrapper.sv 在所有 relevant pre-sim 與 synthesis flow 中都能被穩定找到。
4. pre-sim 與 gate-sim 的 SRAM source policy 已明確且有文件化。

### 5.2 Core local memory 收斂

1. Isram 與 DataSram 不再含有 fake mem[] shadow array。
2. macro Q 成為架構上唯一正式的 read source。
3. local memory read 遵守 1-cycle response 契約。
4. write-mask 行為被保留且有對應驗證。

### 5.3 Core 架構收斂

1. Isram / CoreMcu / CoreController 的介面與行為符合選定的 canonical 架構。
2. 如果選擇 fetch-only Isram，則任何 local load/store traffic 都不再被路由到 Isram。
3. CoreMcu 可以正確承受 synchronous local memory latency。

### 5.4 驗證收斂

1. unit test 不再依賴隱藏的 mem[] internals。
2. firmware regression 不再依賴對 dsram.mem 的 hierarchy peek。
3. 至少有一組 gate-level sanity pass 能確認 wrapper 時間模型與 post-synthesis 行為沒有明顯落差。

---

## 6. 工作分流

## 6.1 工作流 A：Build / Script / Source-Collection 收斂

### 目標

讓所有 simulation 與 synthesis 入口在 wrapper 搬移後，都能以一致方式解析 SRAM source。

### 後續很可能需要修改的檔案

1. design/hybridacc-RTL/Makefile
2. design/hybridacc-RTL/script/syn_hybridacc.tcl
3. design/hybridacc-RTL/script/synthesis_cluster_units.tcl
4. design/hybridacc-RTL/script/synthesis_pe_units.tcl
5. design/hybridacc-RTL/script/synthesis_noc_units.tcl
6. design/hybridacc-RTL/script/add_gate_sim_support.py
7. design/hybridacc-RTL/tb/tb_hybridacc_smoke.sv
8. design/hybridacc-RTL/tb/tb_hybridacc_sim.sv
9. design/hybridacc-RTL/tb/Cluster/cluster_rtl_stack.svh
10. design/hybridacc-RTL/tb/PE/tb_pe_sim.sv
11. 任何直接 include 舊 wrapper path 的 testbench

### 必要動作

#### A1. 統一路徑到 shared wrapper

將所有 active 的舊 path 參考統一改為：

- src/utils/SRAM_Wrapper.sv

#### A2. 將 src/utils 納入 synthesis source discovery

top-level synthesis 與相關 unit synthesis flow 必須明確 analyze src/utils/SRAM_Wrapper.sv，或至少將 src/utils 加入 source pattern collection。

原因是目前多個 PE、Core、Cluster 模組都直接 instantiate TS1N16 macro 名稱，因此分析與 elaboration 時必須明確看得到對應 module 定義。

#### A3. 移除對 Cluster/SRAM.sv 的過時依賴

cluster synthesis script 不應再把 Cluster/SRAM.sv 列為 ScratchpadMemory 與 ComputeCluster 的 dependency。

#### A4. 正式定義 simulation policy

專案需要明確選擇並文件化 pre-sim policy。

##### 建議 policy

1. 預設 pre-sim 可在 foundry model 不可用，或由專用 flag 關閉時，使用 shared wrapper。
2. gate-sim 一律使用 foundry SRAM model。
3. TSMC_SRAM_MODEL_LOADED 的語意保留為「foundry model 已存在」。

##### 為何這件事重要

目前 Makefile 預設直接定義 TSMC_SRAM_MODEL_LOADED 並載入 SRAM_V，這會繞過新的 wrapper-first 意圖，導致所謂「pre-sim behavior model」無法真正落成 build policy。

#### A5. 不要手動修補 generated artifacts

csrc、歷史 build log、sim log 與 report 目錄下的生成物不應手動編修。正確流程應是：

1. 先更新 source 與 script。
2. clean 舊生成物。
3. 重新產生。

### 工作流 A 的退出條件

1. 乾淨的 search 不再找到任何 active source/script 對舊 wrapper 或已刪除 SRAM file 的參考。
2. top-level 與 unit synthesis 都能在不依賴偶然 file ordering 的情況下完成 macro 模組分析。
3. pre-sim 與 gate-sim 的 SRAM source 行為是明確且可重現的。

---

## 6.2 工作流 B：Isram 重構與契約清理

### 目標

將 Isram 收斂成真正 macro-backed 的 local memory block，使其對外可觀察行為符合預期架構與 timing 契約。

### 目前要解決的問題

1. RTL 存在 data-side IRAM port，但 ESL 架構介面沒有。
2. shadow mem[] array 才是功能上真正的資料來源。
3. read path 為 same-cycle。
4. storage 被拆成 fetch / data 兩組 macro bank。

### 建議目標

採 fetch-only Isram。

### 若選擇 fetch-only Isram，必要動作如下

#### B1. 移除 Isram 的 data-side 介面

移除：

1. mcu_dm_valid_i
2. mcu_dm_addr_i
3. mcu_dm_rdata_o

#### B2. 收斂雙重 macro array

將目前的 dual-array 結構改為只保留一組 macro array。

#### B3. 實作真實 fetch timing

instruction fetch 應符合以下行為：

1. cycle N：fetch request 驅動 macro address / CEB。
2. cycle N+1：output data 才視為有效並被消耗。

具體實作可包含：

1. request-valid register。
2. registered address / macro select context。
3. 若需要則加入 response-valid register。

#### B4. 保留 loader write 的 ownership 規則

應維持目前已文件化的規則：

1. load_phase=1：loader 擁有 write path。
2. run phase：loader 不再寫入 active instruction storage。

#### B5. 決定非法 access 的語意

當 data-side IRAM access 被移除後，任何 Core 對 Inst-RAM range 的 load/store 都必須一致地做以下其中一種處理：

1. 以 unsupported access 形式 halt。
2. 以 load/store access fault 形式 trap。

### 若暫時保留 compatibility mode，至少仍需做到

如果專案堅持在過渡期內保留 data-side IRAM read，則至少仍需：

1. 移除 mem[]。
2. 改成 1-cycle timing。
3. 不可在沒有文件的情況下默默保留 duplicated storage。
4. 明確標示這是 temporary、non-canonical path。

### 後續很可能需要修改的檔案

1. design/hybridacc-RTL/src/Core/Isram.sv
2. design/hybridacc-RTL/src/Core/CoreController.sv
3. design/hybridacc-RTL/src/Core/CoreMcu.sv
4. design/hybridacc-RTL/tb/Core/tb_isram.sv
5. 可能還包含 RTL 或 ESL 側的相關文件

### 工作流 B 的退出條件

1. Isram 不再使用 mem[] 作為架構上的正式資料來源。
2. 若選擇 fetch-only，則 Inst-RAM 只保留單一 macro-backed storage path。
3. Isram 對外可觀察 timing 為 1-cycle。
4. data-side IRAM 的行為是否支援，已有明確定義與文件。

---

## 6.3 工作流 C：DataSram 重構

### 目標

讓 DataSram 成為真正 macro-backed 的 local data memory，同時保留正確的 byte-enable semantics 與 1-cycle read behavior。

### 目前要解決的問題

1. shadow mem[] array 是架構上的正式 read source。
2. read path 為 combinational、zero-wait。
3. 現有測試假設 same-cycle read 可見。

### 必要動作

#### C1. 移除 mem[] shadow storage

macro array 必須成為唯一真實的資料來源。

#### C2. 將 byte-lane write 行為收斂到 BWEB

既有 byte / halfword / word semantics 需要保留，但所有 state mutation 都應透過 macro control signal 完成，而不是另外維護 shadow array。

#### C3. 加入 read-response staging

local read 應改為明確的 1-cycle 行為：

1. 在 request issue 時 capture 對齊後的 read context。
2. 下一拍讀取 macro Q。
3. 依據註冊下來的 response context 組出回傳的 32-bit word。

#### C4. 定義同位址 read/write overlap 行為

專案需要明確定義：若 read 與剛發出的 write 落在同一 word 或 lane，read 應看到什麼。

建議做法：

1. 以 wrapper / foundry model 的行為為準。
2. 透過 unit test 直接驗證。
3. 除非明確規格化，否則不要再保留 shadow-array forwarding shortcut。

#### C5. 為測試提供替代的 debug / inspection 手段

因為 mem[] 會消失，所有直接偷看 internal storage 的 testbench 都需要替代手段，例如：

1. 透過正式 public interface 做 readback。
2. 透過 host 可觀測 summary area。
3. 提供受控的 simulation-only debug helper path。

### 後續很可能需要修改的檔案

1. design/hybridacc-RTL/src/Core/DataSram.sv
2. design/hybridacc-RTL/tb/Core/tb_datasram.sv
3. design/hybridacc-RTL/tb/tb_hybridacc_sim.sv
4. 任何直接讀 dsram.mem 的 harness 或 test

### 工作流 C 的退出條件

1. DataSram 不再含有架構上的 mem[] shadow array。
2. byte-enable semantics 仍能通過專門的 unit test。
3. read behavior 已改為 1-cycle，且符合 macro 預期。
4. 預設測試流程中不再依賴 dsram.mem 的 hierarchy access。

---

## 6.4 工作流 D：CoreMcu / CoreController 記憶體協定收斂

### 目標

更新 Core 端邏輯，使其能正確消費 synchronous local memory behavior。

### 為何這是必要工作

這是整份計劃中最關鍵的依賴。

目前 RTL CoreMcu 假設：

1. if_rdata_i 在 fetch address 發出的同一執行 cycle 就可使用。
2. ls_resp_rdata_i 在 load address 發出的同一執行 cycle 就可使用。

因此：

1. 若只改 Isram/DataSram timing 而不改 CoreMcu，執行一定會壞。
2. 單做 memory wrapper cleanup 並不足夠。

### 建議的實作方向

採用明確的 synchronous-memory contract，做法可參考 ESL integration style。

有兩種可行實作形式。

#### D1. 較佳方案：引入明確的 local-memory response-valid 契約

為 local memory path 增加 response-valid semantics，例如：

1. if_resp_valid_i
2. ls_resp_valid_i

優點：

1. timing contract 更明確。
2. 未來 local memory 行為若再變動，擴充性更好。
3. 可減少 CoreController wiring 中的隱含假設。

代價：

1. 介面變更範圍較大。

#### D2. 可接受的短期方案：固定 1-cycle local-memory contract + internal pending state

若想先減少 top-level port churn，可讓 CoreMcu 內部固定假設：

1. local fetch response 永遠在下一拍回來。
2. local load response 永遠在下一拍回來。
3. MMIO 仍維持獨立 valid-qualified。

優點：

1. 對外介面變動較小。

代價：

1. 契約不如顯式 valid 來得清楚。
2. 後續若再擴充會比較辛苦。

### 若採 fetch-only Isram，還需追加的架構清理

#### D3. 將 Inst-RAM 移出合法 load/store target

目前 RTL CoreMcu 與 CoreController 明確把 Inst-RAM 視為合法的 local load 範圍；若採 fetch-only Isram，這段必須移除。

#### D4. 定義非法 access 的處理行為

當 software 對 Inst-RAM range 發出 load/store 時，設計需要明確定義是：

1. deterministic halt。
2. architected fault / trap。

#### D5. 更新相關文件

任何目前仍將「ISRAM rodata load」描述為 canonical 行為的 RTL 文件，都必須同步修正。

### 後續很可能需要修改的檔案

1. design/hybridacc-RTL/src/Core/CoreMcu.sv
2. design/hybridacc-RTL/src/Core/CoreController.sv
3. design/hybridacc-RTL/tb/Core/tb_corecontroller_smoke.sv
4. design/hybridacc-RTL/tb/tb_hybridacc_sim.sv
5. design/hybridacc-RTL/doc/implement_plan.md
6. 可能還包含 ESL 側對齊文件或 callback 設定

### 工作流 D 的退出條件

1. CoreMcu 不再依賴 same-cycle local memory read data。
2. CoreController 的 memory routing 符合選定的 Inst-RAM 契約。
3. local memory 與 MMIO 行為都明確且可預測。
4. firmware smoke test 仍可正常開機並退休指令。

---

## 6.5 工作流 E：ScratchpadMemory Timing Audit

### 目標

確認 ScratchpadMemory 現有設計本身已大致符合 macro-backed timing，並只針對真正不一致的區塊做局部修補。

### 重要澄清

這不是一個「移除 fake array」的任務。目前在 ScratchpadMemory 中並未發現 shadow mem[] array。

### 必查項目

#### E1. NOC read path

需要確認：

1. 被接受的 read request 會 capture 正確的 bank/row context。
2. response 只會由正確延遲後的 sram_q data 組成。
3. backpressure 不會破壞 response retention。

#### E2. Parallel read path

需要確認 multi-bank parallel read：

1. 會 capture 正確的 group context。
2. 會在下一拍從正確的 macro Q word 取值。
3. 在 backpressure 與 arbitration 下仍然正確。

#### E3. DMA read path

需要確認 AXI read response：

1. 是由正確延遲後的 bank/row context 所產生。
2. 沒有錯誤使用到同拍 address 資訊。
3. 在與 NOC traffic 競爭時仍維持正確 arbitration。

#### E4. Write/read collision semantics

對於以下 bank-conflict 或 same-row overlap：

1. NOC write vs NOC read。
2. NOC write vs DMA read。
3. DMA write vs NOC read。

都應明確定義並驗證其可觀察行為。

### 後續很可能需要修改的檔案

1. design/hybridacc-RTL/src/Cluster/ScratchpadMemory.sv
2. 針對 SPM timing 的 cluster testbench

### 工作流 E 的退出條件

1. ScratchpadMemory 的 timing 假設有被明確驗證。
2. 若找到 bug，可在不引入 shadow storage 的前提下局部修補。
3. 既有 cluster tests 持續通過，或在 expectation 改動合理時同步更新。

---

## 6.6 工作流 F：Testbench 重構與驗證清理

### 目標

讓測試改為驗證 architected behavior，而不是驗證偶然存在的 internal implementation detail。

### 目前已知需要移除的測試依賴

#### F1. tb_isram 目前仍在驗證 data-side IRAM read

這反映的是目前 RTL 的相容路徑，而不是建議採用的 canonical 架構。

若採 fetch-only Isram：

1. 移除 data-side IRAM read 相關檢查。
2. 改為驗證 fetch latency 與 loader write 行為。
3. 如有需要，另外新增非法 access 行為測試。

#### F2. tb_datasram 目前假設 same-cycle read 可見

這必須改成下一拍觀察。

#### F3. tb_hybridacc_sim 直接讀取 dut.core_ctrl.dsram.mem

在 mem[] 移除後，這段一定會壞。

應改成以下其中一種：

1. 透過正式 public interface 做 readback。
2. 只透過 firmware 寫出的 summary area 驗證。
3. 提供不依賴 shadow storage 的 simulation-only dump helper。

#### F4. tb_sram 仍引用已刪除的 Cluster/SRAM.sv

這個測試不能維持現況。

可接受的處理方式只有兩種：

1. 將其自預設測試流程退役。
2. 將其改寫成 shared wrapper unit test 或 focused ScratchpadMemory timing test。

#### F5. 其他直接偷看 internal mem[] 的測試

凡是依賴 hierarchy-level mem[] access 的 test，都應視為脆弱測試，並逐步轉換成透過 architected observation point 驗證。

### 後續需落實的驗證矩陣

#### Unit-level

1. tb_isram
2. tb_datasram
3. focused ScratchpadMemory timing test，或 tb_sram 的替代方案

#### Core integration

1. tb_corecontroller_smoke
2. 若移除 Inst-RAM data-side access，則追加 targeted trap / illegal-access test

#### Top-level firmware validation

1. tb_hybridacc_sim
2. 至少一組小型 RTL firmware regression case

#### Gate / post-synthesis correlation

1. 一組 Core-side block sanity check
2. 一組 cluster-side block sanity check
3. 若 runtime 可接受，再加一組 top-level spot check

### 工作流 F 的退出條件

1. 預設測試不再需要 mem[] hierarchy peek。
2. unit 與 integration test 會正確驗證 1-cycle memory contract。
3. regression harness 在 shadow-array 移除後仍可用。

---

## 7. Isram 與 ESL 差異盤點

本節對應原先第三項需求：找出目前 RTL Isram 與 ESL model / contract 的差異，並提出改善方向。

### 7.1 介面差異

#### 目前 RTL Isram

1. 有 fetch-side interface。
2. 也有 data-side interface。
3. 有 loader-write interface。

#### ESL Isram 契約

1. 有 fetch-side interface。
2. 有 loader-write interface。
3. 沒有 public data-side IRAM load interface。

#### 結論

目前 RTL Isram 的介面比 ESL 架構契約更寬。

### 7.2 儲存結構差異

#### 目前 RTL Isram

1. shadow byte array mem[]
2. fetch macro array
3. data-side macro array

#### ESL Isram model

1. 單一 SRAM backend abstraction
2. 沒有對外暴露額外 data-side Inst-RAM port

#### 結論

目前 RTL Isram 的結構更重，也比較不 canonical。

### 7.3 Timing model 差異

#### 目前 RTL Isram

1. read data 由 mem[] 以 combinational 方式組出。
2. current address 當拍就可見。

#### ESL 文件契約

1. load delay 預設為 1 cycle。

#### 結論

目前 RTL Isram timing 與文件定義的目標契約不一致。

### 7.4 Software contract 差異

#### 目前 RTL 文件

部分 RTL 規劃文件仍將 ISRAM rodata load 視為支援行為。

#### hybridacc-cc 契約

compiler/linker 文件已將 .rodata 放在 DSRAM，而非 ISRAM。

#### 結論

目前 RTL 行為已偏離 software memory layout 契約。

### 7.5 建議的 Isram 改善方向

1. 將 data-side Inst-RAM access 自 canonical 架構移除。
2. 將 Isram 收斂成單一 macro-backed fetch memory。
3. 將 fetch response 收斂為 1-cycle semantics。
4. 同步調整 CoreController / CoreMcu 與測試。
5. 更新文件，避免後續驗證延續目前的模糊狀態。

---

## 8. 建議的實作順序

這些工作不應該任意穿插進行。

### Phase 0：先凍結決策

在修改 RTL 前，先明確決定以下三件事：

1. Isram 的 canonical mode 是 fetch-only 還是 compatibility mode。
2. 非法 Inst-RAM load/store 要以 halt 還是 trap 處理。
3. Core local-memory protocol 要採 explicit valid 還是固定 1-cycle implicit contract。

### Phase 1：先做 source 與 build 收斂

先完成工作流 A，確保後續所有 simulation / synthesis run 都是基於正確 source tree。

### Phase 2：重構 Isram 與 DataSram

接著做工作流 B 與 C，但此時不應期待所有測試馬上通過。

### Phase 3：緊接著收斂 CoreMcu / CoreController

在 local memory timing 改變後，必須立刻做工作流 D。這是恢復功能執行的關鍵依賴。

### Phase 4：再清理 testbench

等介面穩定後，再做工作流 F，避免測試在介面尚未定案前反覆重寫。

### Phase 5：最後做 ScratchpadMemory audit 與 cluster 驗證

待 Core 端收斂穩定後，再做工作流 E 與 focused regression。

### Phase 6：最後才進 synthesis 與 gate correlation

只有在前述 phases 都完成後，才適合進行 synthesis 與 gate-sim signoff 檢查。

---

## 9. 風險登錄

### 風險 1：CoreMcu retiming 造成 firmware 執行大範圍失效

為何重要：

目前 CoreMcu 對 local memory 具有隱含 zero-wait 假設。

降低風險的方式：

1. 先透過 unit test 分階段推進。
2. 在大範圍功能修改前，先加入 deterministic pending-state handling。
3. 先用 tb_corecontroller_smoke 驗證，再進 top-level firmware regression。

### 風險 2：實際上仍存在隱藏的 data-side IRAM 軟體依賴

為何重要：

移除 ISRAM data-side access 可能打到未知路徑。

降低風險的方式：

1. 以 linker contract 為主，但必要時要驗證。
2. 若失敗出現，再回頭 audit firmware image 或 load/store 使用情況。
3. 若確有需要，可短期保留有文件的 compatibility mode。

### 風險 3：wrapper semantics 與 foundry model semantics 不一致

為何重要：

可能導致 pre-sim 通過但 gate-sim 失敗。

降低風險的方式：

1. 明確定義 wrapper 的意圖與邊界。
2. 至少拿一小組 unit matrix 同時驗證 wrapper pre-sim 與 foundry-backed gate-sim。
3. 避免引入無法映射到真實 macro 的 behavioral shortcut。

### 風險 4：testbench 反而成為主要阻塞點

為何重要：

目前多個測試都依賴 mem[] hierarchy introspection。

降低風險的方式：

1. 儘早盤點這些測試。
2. 提前提供替代 observation path。
3. 不要把 test refactor 拖到最後一刻。

### 風險 5：source-path cleanup 在不同 flow 上修得不一致

為何重要：

可能出現某個 flow 已修好、另一個 flow 仍偷偷使用舊檔的情況。

降低風險的方式：

1. 盡量集中管理 wrapper path 定義。
2. 每次 cleanup 後都重新 search source 與 script tree。
3. 驗證時一律從乾淨 build directory 開始。

---

## 10. 合成前進場條件

在這輪 memory-converged RTL 尚未滿足以下條件前，不應開始新的 synthesis。

### 10.1 Source tree 條件

1. 不再有任何 active source/script 參考 PE/SRAM_SP_BWEB.sv。
2. 不再有任何 active source/script 參考已刪除的 Cluster/SRAM.sv。
3. src/utils/SRAM_Wrapper.sv 已被納入所有 relevant pre-sim 與 synthesis flow。

### 10.2 Core memory 條件

1. Isram 與 DataSram 已完全改為 macro-backed state。
2. local memory read 已遵守預定的 1-cycle behavior。
3. CoreMcu 已不再依賴 same-cycle local memory visibility。

### 10.3 架構條件

1. Isram 的 canonical 契約已凍結且在 RTL 中一致實作。
2. CoreController routing 已符合該契約。
3. RTL 文件已不再把過時的 ISRAM data-side 行為描述為 canonical。

### 10.4 測試條件

1. tb_isram 已依新契約通過。
2. tb_datasram 已依新契約通過。
3. tb_corecontroller_smoke 已通過。
4. tb_hybridacc_sim 已不再依賴 dsram.mem hierarchy peek。
5. 已為舊 tb_sram 行為提供替代 coverage。

### 10.5 Synthesis / gate sanity 條件

1. 至少一個 Core-side synthesis target 可乾淨完成。
2. 至少一個 cluster-side synthesis target 可乾淨完成。
3. top-level synthesis 可在更新後的 source collection 下完成。
4. selected gate-sim spot checks 未顯示明顯的 memory-contract mismatch。

---

## 11. 完成定義

只有當以下這個系統層級敘述成立時，本計劃才算真正完成：

HybridAcc RTL、其 testbench、以及其 synthesis / simulation script，全都對齊到同一套 SRAM 架構；在這套架構中：

1. 當 foundry model 未使用時，src/utils/SRAM_Wrapper.sv 是共用的 behavioral pre-sim model。
2. gate-sim 一律使用真實 foundry model。
3. Core local memory 的行為是 synchronous macro，而非 same-cycle fake array。
4. Inst-RAM 的 ownership 與 accessibility 在架構上明確定義。
5. 驗證依據的是 architected behavior，而不是 accidental implementation detail。

只有在上述敘述成立時，啟動 synthesis 才具技術上的正當性。


## 注意事項
- vcs 等 tool 要使用 tcsh 環境

---

## Resume 快照

日期：2026-04-30 03:27

本節是給長時間 synthesis / 中斷後 resume 用的工作快照；若之後要喚醒 Copilot 繼續，請優先參考本節。

### 已完成的 RTL / TB 收斂

1. 已完成 Phase 1 的 path / build 收斂。
2. Makefile 已切成 wrapper-first pre-sim policy，並保留 `USE_FOUNDRY_SRAM_MODEL=1` 可切回 foundry model。
3. top / cluster / PE / NoC synthesis script 已加入 `src/utils/SRAM_Wrapper.sv` 路徑。
4. 已清理主線 testbench / helper 對舊 `PE/SRAM_SP_BWEB.sv` 與 `Cluster/SRAM.sv` 的 active 依賴。
5. `Isram.sv` 已改為 fetch-only、single macro-backed、1-cycle response-valid 介面。
6. `DataSram.sv` 已改為 macro-backed、1-cycle response-valid 介面，並提供 simulation-only `debug_read_word()`。
7. `CoreMcu.sv` / `CoreController.sv` 已改為 explicit valid flow，並將 Inst-RAM data-side load/store 視為 unsupported access 後 halt。
8. `tb_hybridacc_sim.sv` 已改成使用 `dut.core_ctrl.dsram.debug_read_word(...)`，不再偷看 `dsram.mem`。
9. 舊的 `tb/Cluster/tb_sram.sv` 已刪除；目前 cluster memory coverage 以 `tb_spm.sv` 為主。

### 已通過的 focused validation

以下指令都已在 tcsh 環境下跑過且通過：

1. `make sim_tb_datamemory`
2. `make sim_tb_isram`
3. `make sim_tb_datasram`
4. `make sim_tb_corecontroller_smoke`
5. `make sim_tb_hybridacc_sim`
6. `make sim_tb_spm`

### synthesis / gate sanity 的最新狀態

1. top-level synthesis 曾經卡在 `Isram.sv` / `DataSram.sv` 的 Presto 相容性語法，該問題已修正。
2. unit synthesis script 曾經把 `$sram_wrapper` 當成字面值而非展開路徑，該問題已修正。
3. top-level synthesis 後續曾卡在 `ComputeCluster.sv` 的 named assignment pattern port connection：
   - 原位置：`noc_plo_in_data('{addr: hddu_noc_plo_addr})`
   - 已改為顯式 struct signal `hddu_noc_plo_req_data`
4. `AddressGenerateUnit.sv` 的 `error_reg_reg` latch root cause 已修正：`error_reg` 原本只在 reset 中被賦值、平常不更新，現在已改成固定為 0 的 status bit，不再保留假暫存器。
5. `CmdFabric.sv` 的 `boot_reason_reg_reg` latch root cause 已修正：`boot_reason_reg` 原本只在 reset 中被賦值、平常不更新，現在已改為 `LOCAL_BOOT_REASON` 直接回傳常數 `32'h0`。
6. 已完成 focused DC 驗證：
   - `tcsh -fc 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL && make syn_cluster_AddressGenerateUnit'`
   - `tcsh -fc 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL && mkdir -p build report/CmdFabric && cp script/synopsys_dc.setup build/.synopsys_dc.setup && cd build && dc_shell -no_home_init -x "analyze -format sverilog ../src/Core/core_pkg.sv; analyze -format sverilog ../src/Core/CmdFabric.sv; elaborate CmdFabric; current_design CmdFabric; link; quit" |& tee ../report/CmdFabric/syn_elab_CmdFabric.log'`
7. 上述 focused log 中已不再出現 `ELAB-978`、`Latch`、`error_reg_reg`、`boot_reason_reg_reg`。
8. `CmdFabric.sv` 仍存在既有的 `ELAB-312` warning（`cluster_mask_hi_i[-32]` out-of-bounds bit select），本輪未處理；若 top-level synthesis 後續仍受影響，下一輪優先修這個 decode/loop boundary 問題。
9. `gate_sim_tb_datamemory` 尚未重新驗證，待 top-level synthesis checkpoint 穩定後繼續。

### 目前正在背景執行的長時間合成

舊 session `hacc_syn_top_20260430` 已終止，避免沿用舊 log 狀態。現在已重新用 tmux 啟動 top-level synthesis：

1. session 名稱：`hacc_syn_top_20260430_v2`
2. 工作目錄：`/home/easonyeh/hybridacc/design/hybridacc-RTL/build`
3. 執行內容：`tcsh -fc 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL && mkdir -p build && cp script/synopsys_dc.setup build/.synopsys_dc.setup && cd build && dc_shell -no_home_init -f ../script/syn_hybridacc.tcl |& tee syn_compile_hybridacc.log'`
4. 最新觀察：session 已成功啟動，`build/syn_compile_hybridacc.log` 已開始重新寫入 Presto compile 輸出。

### resume 時先做什麼

若要接著處理，建議按以下順序：

1. 先檢查 tmux session 是否仍在：`tmux ls | grep hacc_syn_top_20260430_v2`
2. 若要直接觀看執行中輸出：`tmux attach -t hacc_syn_top_20260430_v2`
3. 若只看 log 尾端：`tail -n 80 /home/easonyeh/hybridacc/design/hybridacc-RTL/build/syn_compile_hybridacc.log`
4. synthesis 結束後，先確認 top-level 是否已清掉本輪已修正的 latch warning，並檢查是否仍有 `ELAB-312` 或新的 DC error / unresolved reference。
5. 若 top-level synthesis clean，再跑：
   - `tcsh -fc 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL && make syn_cluster_ScratchpadMemory'`
   - `tcsh -fc 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL && make syn_pe_DataMemory'`
   - `tcsh -fc 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL && make gate_sim_tb_datamemory MOD_NAME=DataMemory'`

### 喚醒 Copilot 時可直接貼的摘要

可直接貼這段作為 resume prompt：

「請延續 plan.md 尾端的 Resume 快照。Phase 1-5 已完成且 `sim_tb_datamemory` / `sim_tb_isram` / `sim_tb_datasram` / `sim_tb_corecontroller_smoke` / `sim_tb_hybridacc_sim` / `sim_tb_spm` 都已通過。`AddressGenerateUnit.sv` 的 `error_reg_reg` 與 `CmdFabric.sv` 的 `boot_reason_reg_reg` latch 已修掉，focused DC 驗證已不再出現 `ELAB-978` / `Latch`。現在 tmux session `hacc_syn_top_20260430_v2` 正在重跑 top-level `syn_hybridacc.tcl`。請先讀 `build/syn_compile_hybridacc.log` 與 tmux 狀態，確認 synthesis 結果，再接著完成 cluster / PE synthesis、追蹤 `CmdFabric.sv` 的 `ELAB-312`、以及 gate sanity。」