# Core ISA 設計比較：`core_ISA_v2` vs `core_ISA_rv32I` vs `core_ISA_HACC`

本文比較三種 Core Controller 設計方向，聚焦在：

1. 工具鏈與可維護性
2. 硬體實作複雜度
3. 大量 wave（數千～數萬）可擴展性
4. 與現有 HybridAcc MMIO/descriptor 架構的整合成本

---

## 1) 一頁式比較表

| 比較面向 | `core_ISA_v2`（自定義 Core-ISA） | `core_ISA_rv32I`（RV32I + XHACC） | `core_ISA_HACC`（Native HACC ISA） |
|---|---|---|---|
| 核心定位 | 通用控制 ISA + pseudo 指令 | 標準 CPU ISA + 少量加速擴充 | 任務提交型控制前端（非通用 CPU） |
| 指令生態 | 需自建 assembler/debug | 可沿用 RISC-V 工具鏈（最佳） | 完全客製工具鏈 |
| 軟體可攜性 | 低 | 高 | 低 |
| 硬體設計自由度 | 中 | 中（受 RV32I pipeline/ABI 約束） | 高（可完全為 workload 最佳化） |
| 首版落地風險 | 中 | 低～中（最穩健） | 中～高（硬體/工具都客製） |
| 大量 wave code size | 透過 wave loop + desc 可壓到 O(1) | 同樣可 O(1)，且語義最清楚 | 天生 O(1)（core stub 最短） |
| 大量 wave runtime 開銷 | 中（視 pseudo lowering） | 中～低（可逐步導入 XHACC） | 低（波次由硬體執行複合流程） |
| 除錯可觀測性 | 中（需自建 trace） | 高（可用標準 debug + 自訂 trace） | 中（依賴設計良好的 CSR/事件） |
| 對 MMIO map 相容性 | 高 | 高 | 高 |
| 對既有文件一致性 | 高 | 高 | 高 |
| 擴充策略 | 以 pseudo/macros 演進 | 標準 ISA 為主，XHACC 漸進 | capability bitmap + unit 升級 |
| 長期維護成本 | 中～高 | 低（最佳） | 中～高 |
| 極致效能上限 | 中～高 | 高（取決於 XHACC 深度） | 高（可專用化到極致） |
| 適合團隊型態 | 小團隊快速原型 | 多人協作、長期產品化 | 硬體主導、追求專用加速 SoC |

---

## 2) 各方案優缺點分析

## 2.1 `core_ISA_v2`（自定義 Core-ISA）

### 優點

1. **語意貼近需求**：`CL.MMIO.*`、`WAVE.*`、`DMA.*` 直接對應 cluster/dma/wave 控制。
2. **與現有架構銜接快**：延續目前 MMIO 分區與控制流程，migration 成本低。
3. **可先軟後硬**：pseudo 可先由 assembler/compiler 展開，不必一次做完硬體 macro。
4. **已納入大 wave 策略**：明確要求 loop + descriptor，避免 `SEC_CORE_PROGRAM` 爆長。

### 缺點

1. **工具鏈孤島**：assembler、反組譯、除錯器、ABI 都要自建與維護。
2. **生態重用差**：很難直接使用既有編譯器最佳化與 debug 工具。
3. **規格演進風險**：opcode/語意若持續變動，軟硬體版本相容成本上升。
4. **跨團隊溝通成本較高**：新成員需先熟悉客製 ISA。

### 總評

`core_ISA_v2` 適合「已經有自定義控制流、希望最短路徑整合」的階段；但若要長期產品化，工具鏈成本會逐漸成為主負擔。

---

## 2.2 `core_ISA_rv32I`（RV32I Base + XHACC）

### 優點

1. **標準化最佳**：RV32I ABI/工具鏈完整，程式生成、debug、維護最友善。
2. **風險可分階段控制**：先「純 RV32I + MMIO」可跑，再逐步加入 `hacc.clw/drange/wexec`。
3. **兼顧彈性與效能**：通用控制流用 RV32I，熱路徑再用少量 custom 指令加速。
4. **最適合多人協作**：文件、工具、生態都成熟，知識傳承容易。
5. **同樣支持大 wave O(1) code**：可維持小型 loop + descriptor table 架構。

### 缺點

1. **硬體仍有複雜度**：若做 XHACC，需要 decoder、scoreboard、sequencer 等整合。
2. **兩層語意管理**：RV32I 路徑與 XHACC 路徑需保持功能等價，驗證量增加。
3. **極端專用化不如純 Native**：若追求最短控制延遲，仍受通用 CPU 模型限制。

### 總評

`core_ISA_rv32I` 是三者中最平衡、工程風險最低、長期維護最佳的方案；通常是研究原型走向可持續平台時的首選。

---

## 2.3 `core_ISA_HACC`（Native HACC ISA）

### 優點

1. **專用化程度最高**：ISA 直接面向 Job/Wave 提交，控制碼極短且語意集中。
2. **硬體排程效率高**：wave 微流程全由硬體執行，理論上 runtime overhead 最低。
3. **架構一致性強**：以 descriptor 驅動為核心，天然符合大 wave 需求。
4. **可做深度 SoC 最佳化**：對 command queue、profile cache、descriptor prefetch 可高度定製。

### 缺點

1. **工具鏈成本最高**：需自建 assembler/debug/trace/驗證生態。
2. **設計與驗證風險最高**：CPU-like 前端 + 多個 execution units + 錯誤模型都要自行定義。
3. **軟體可重用低**：不易重用通用編譯器、ABI、第三方工具。
4. **人員門檻高**：硬體與編譯器團隊都需長期投入。

### 總評

`core_ISA_HACC` 適合「硬體主導且追求極致專用化」的中長期路線；不建議作為最初期 MVP 的第一步。

---

## 3) 針對你的專案目標的決策建議

若目標是**近期可落地 + 中期可擴展 + 長期可維護**，建議優先順序：

1. **主路線：`core_ISA_rv32I`**
	- 先完成純 RV32I + MMIO 的 wave loop 架構。
	- 確保 `SEC_WAVE_DESC / SEC_PROFILE_TABLE / SEC_DMA_DESC` 與 loader 全流程穩定。

2. **性能增強：引入最小 XHACC**
	- 第一批：`hacc.clw`、`hacc.drange`。
	- 第二批：`hacc.wload/wexec/wnext`。

3. **保留 Native HACC 作為遠期方案**
	- 將 `core_ISA_HACC` 視為「專用控制平面 v2/v3」研究路線。
	- 以 capability bitmap 與 section 相容為前提，避免未來重寫工具鏈。

---

## 4) 總結

1. **要最快做出可用且可維護系統**：選 `core_ISA_rv32I`。
2. **要短期沿用既有設計語彙**：`core_ISA_v2` 可作過渡，但應避免長期鎖死。
3. **要追求最專用、最高控制效率**：`core_ISA_HACC` 潛力最大，但初期成本最高。

在你目前「cluster + DMA 協同、且 wave 數可到數千以上」的情境下，三者共同的關鍵原則都一致：

**固定小型 core 程式 + descriptor 資料驅動 + profile/delta 更新**。
