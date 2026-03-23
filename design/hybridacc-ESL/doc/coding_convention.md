# hybridacc-ESL Simulator Coding Convention

## 1. 目的與適用範圍

本文件整理 `design/hybridacc-ESL/simulator` 既有程式風格與建議實務，作為後續新增/修改模組時的統一依據。

適用範圍：

- `simulator/include/**/*.hpp`
- `simulator/src/**/*.{cpp,cc,cxx}`

原則：

1. 先遵守現有風格，避免無關重構。
2. 同一檔案內保持一致，跨檔案儘量對齊。
3. 以可讀性、可追蹤性、可驗證性為優先。

---

## 2. 檔案與命名規則

### 2.1 檔案與目錄

- Header 使用 `.hpp`，source 使用 `.cpp`。
- 依功能分目錄：`Cluster/`、`NoC/`、`PE/`、`Core/`、`AXI4_lite/`。
- 模組名與檔名一致，例如 `ScratchpadMemory.hpp` 對應 `SC_MODULE(ScratchpadMemory)`。

### 2.2 型別、常數、變數命名

- 類別/模組/結構：`PascalCase`（例：`ScratchpadMemory`, `NoCRouter`）。
- 函式：`snake_case`（例：`main_process`, `ready_process`, `trace_process`）。
- 常數：
	- compile-time 常數使用 `kPrefix`（例：`kNumBanks`, `kSpmDataBits`）。
	- `enum class` 使用大寫列舉值（例：`SPM_OK`, `SPM_ERROR`）。
- 訊號/暫存命名後綴（強制建議）：
	- `_i` / `_o`：外部輸入/輸出 port。
	- `_sig`：內部 `sc_signal`。
	- `_reg`：序向狀態寄存。
	- `_next`：next-state 暫存（若使用）。
	- `_q_reg`：queue/deque 型暫存。

### 2.3 模板參數

- 模板參數用全大寫語意名稱（例：`NUM_NOC_CHANNEL`, `SRAM_BANK_DEPTH_WORDS`）。
- 提供保守預設值，且需能對應既有測試配置。

---

## 3. SystemC 模組撰寫規範

### 3.1 基本結構順序（建議）

1. `#include` 與 namespace。
2. `SC_MODULE` 宣告與 `static constexpr` / `using`。
3. `enum/struct` 區。
4. Ports。
5. Internal signals / registers / queues。
6. Constructor（含 bind、process 註冊）。
7. Destructor（若有動態配置）。
8. private methods。

### 3.2 Process 規範

- 至少區分：
	- 一個序向 process（通常 `SC_CTHREAD`）負責狀態更新。
	- 一到多個組合 process（`SC_METHOD`）負責輸出與 next-state 計算。
- process 命名要反映功能：
	- `*_process`、`comb_*`、`seq_*`、`trace_process`。
- `sensitive` 列表需完整覆蓋讀取來源，避免隱性 latch-like 行為。

### 3.3 Reset 規範

- 模組 reset 以 `reset_n`（active-low）為主，統一使用：
	- `SC_CTHREAD(..., clk.pos());`
	- `reset_signal_is(reset_n, false);`
- reset 區塊必須明確初始化：
	- 有效/就緒訊號（valid/ready）。
	- 指標、計數器、queue/FIFO、暫存資料。
- 若有子系統局部 reset（如 PMU reset），需明確標示「只影響哪些狀態」。

---

## 4. 介面與握手規範

### 4.1 Valid/Ready 介面

- 一律採 valid/ready 語意：
	- transaction fire 條件為 `valid && ready`。
- 遇到背壓時，資料需保持穩定直到被接收。
- 建議使用 skid buffer 或 FIFO 消除長組合路徑。

### 4.2 FIFO 使用

- FIFO depth 由建構子參數注入，禁止硬編碼 magic number。
- 明確處理 full/empty 邊界條件：
	- full 且 push（無 pop）應告警或丟棄（需文件化）。
	- empty 且 pop 應告警或忽略（需文件化）。

### 4.3 AXI4-Lite / SPM 介面

- 命名依協定維持一致（`aw/w/b/ar/r`）。
- `*_resp`/`*_code` 欄位必須定義 enum 與語意（例如 OK/ERROR）。
- 規格文件需與實際 payload 結構同步更新。

---

## 5. C++ 寫作與可維護性

### 5.1 Include 與 namespace

- Header 一律使用 `#pragma once`。
- include 順序建議：
	1. 標準函式庫
	2. SystemC
	3. 專案內部 headers
- 目前程式大量使用 `using namespace sc_core;` / `sc_dt;`，新檔可沿用；
	若新增通用工具檔，優先使用限定名稱避免污染。

### 5.2 型別與容器

- 位寬敏感資料優先使用 `sc_uint` / `sc_biguint`。
- queue 語意使用 `std::deque`；固定大小集合用 `std::array`。
- 避免裸指標；若因 SystemC/模組生命週期必須使用，需提供對應 destructor 釋放。

### 5.3 比較與追蹤支援

- 給 `sc_signal` 承載的自定義型別，應實作：
	- `operator==`
	- `operator<<`
	- `sc_trace(...)`
- enum 也應提供 `operator<<` 與 `sc_trace`，便於波形與 log 除錯。

---

## 6. Debug、Trace 與錯誤處理

### 6.1 Debug 訊息

- 使用既有 `DEBUG_MSG` / `DEBUG_PRINTF`，避免散落 `std::cout`。
- debug level 應選擇對應子系統層級（PE / NOC / CLUSTER / TOP）。

### 6.2 Trace

- 每個主要模組保留 `trace_process`。
- 重要狀態切換（FSM、stall、valid-ready event）應可被 trace。
- thread/event 命名應固定可搜尋，避免臨時字串。

### 6.3 錯誤與警告

- 可恢復錯誤使用 `SC_REPORT_WARNING` 並清楚描述條件。
- 不可恢復或設計假設違反使用 `assert`（僅針對設計 invariant）。
- Out-of-bound、非法配置、協定違反等行為需明文化（忽略/回錯/清零）。

---

## 7. 註解與文件同步

### 7.1 註解風格
- 使用英文註解，保持專業與一致性。
- 參考 Doxygen 風格，重要模組/方法需提供簡要說明。
- 優先描述「為什麼」而非「做了什麼」。
- 複雜資料流/時序需有區塊註解（stage、flow、hazard）。
- 可中英混寫，但同一區塊保持一致且術語統一。

### 7.2 規格與程式一致性

- 任何介面變更（位寬、欄位、reset 行為、優先權）必須同步更新規格文件。
- merge 前至少檢查：
	1. `doc/SPM_v*.md` 與 header 中 port/enum 一致。
	2. default 參數與測試配置一致。
	3. response code 與錯誤語意一致。

---

## 8. 新增模組 Checklist（提交前）

- [ ] 命名符合 `_i/_o/_sig/_reg/_next` 規範。
- [ ] reset 行為完整，無未初始化寄存。
- [ ] valid/ready 握手在背壓下資料保持穩定。
- [ ] 自定義型別補齊 `==`、`<<`、`sc_trace`。
- [ ] 重要狀態具備 trace/debug 可觀測性。
- [ ] 規格文件已同步（若涉及介面/語意變更）。
