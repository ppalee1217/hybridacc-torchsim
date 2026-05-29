# Utilization Profile Report

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

## 範圍

- 這份筆記整理目前 wave-gap profiling flow 下，utilization 為什麼會呈現這樣的趨勢；分析基準以目前 source tree 對應的 simulator raw log 為主。
- 主要回答兩個問題：
  - 為什麼扣掉 wave-gap 工作後，`Ideal HW/SW Co-design MACs Utilization (%)` 不是固定值。
  - conv1x1 裡看到的 `-73` 截距到底從哪裡來。
- 這次交叉檢查了三類 workload：
  - conv1x1 IC sweep
  - conv3x3 IC sweep
  - gemm M/N sweep

## 方法

- simulator 端的語意以目前實作為準，主要看：
  - [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp](../../simulator/include/Core/CoreController.hpp#L1093)
  - [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp](../../simulator/include/Core/CoreController.hpp#L1135)
- utilization 的計算公式以 Python 報表邏輯為準，位置在 [python/hybridacc_cc/sweep_tools.py](../../../../python/hybridacc_cc/sweep_tools.py#L856)。
- 趨勢擬合時，我直接使用 raw `sim.log`，來源目錄為：
  - [output/analysis-wave-gap/conv1x1_ic_sweeps](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps)
  - [output/analysis-wave-gap/conv3x3_ic_sweeps](../../../../output/analysis-wave-gap/conv3x3_ic_sweeps)
  - [output/analysis-wave-gap/gemm_mn_sweeps](../../../../output/analysis-wave-gap/gemm_mn_sweeps)
- 我沒有直接把既有的 `sweep_metrics.csv` 當成絕對真值，因為至少有一部分目前 report 目錄裡的數值，和當前 raw `sim.log` 對不上。

## 資料品質註記

現有 summary CSV 和目前 raw log 並沒有完全同步。

代表性的 conv1x1 例子如下：

- Case：`conv1x1_ic_oh16_ow64_ic24_oc48`
- raw `sim.log` 顯示：
  - `[SIM] Cluster boot-up cycles: 26451`
  - `[SIM] Cluster boot-up detail: start_cycle=1 first_start_cycle=26451 cycles=26451 ...`
  - `[SIM] Cluster drain-out detail: last_stop_cycle=35897 end_cycle=49581 cycles=13685 ...`
- 但 [output/analysis-wave-gap/conv1x1_ic_sweeps_report/sweep_metrics.csv](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps_report/sweep_metrics.csv) 記的是：
  - `boot_up_cycles = 25678`
  - `steady_state_core_cycles = 10218`

如果依照當前 raw log 直接重算，這個 case 應該是：

$$
steady = 49581 - 26451 - 13685 = 9445
$$

CSV 比 raw log 大了剛好 `773`，也就是一個 conv1x1 的 wave-gap window。

其他 multi-wave report 也有相同型態的偏差：

- conv3x3 `ic=8`：CSV 的 steady-state 比 raw 大 `645`，剛好等於一個 conv3x3 gap window。
- gemm `m=48, n=256`：CSV 的 steady-state 比 raw 大 `677`，大致等於該 case 的平均 gap window。

所以這份報告以下的數字，以 raw log 為目前真值。

## 核心公式

Python 端的 utilization 定義是：

$$
U = 100 \cdot \frac{MACs}{cycles \cdot active\_pes \cdot 4}
$$

而所謂的 "ideal" 分母，不是：

$$
steady\_state\_core\_cycles - wave\_gap\_cycles\_total
$$

它其實是：

$$
ideal\_hw\_sw\_codesign\_core\_cycles = steady\_state\_core\_cycles - wave\_gap\_data\_compute\_instructions\_total
$$

這就是第一個關鍵原因：這個 metric 扣掉的只有 gap 裡的 `data_compute` instructions，不是整個 gap 的時間。

為了後面描述方便，本文固定再引入三個記號：

$$
C_{steady}(x) = steady\_state\_core\_cycles(x)
$$

$$
C_{gap,data}(x) = wave\_gap\_data\_compute\_instructions\_total(x)
$$

$$
C_{ideal}(x) = C_{steady}(x) - C_{gap,data}(x)
$$

其中 $x$ 代表 sweep workload 變數，例如 conv1x1/conv3x3 的 IC，或 gemm 的某一個 sweep 軸。

## 指標命名固定

為了避免把不同語意的 ideal utilization 混成同一個詞，本文之後固定使用下列三個名稱。

### 1. 累積理想利用率

英文：`Cumulative Ideal Utilization`

$$
U_{ideal,cum}(x) = 100 \cdot \frac{MACs(x)}{C_{ideal}(x) \cdot active\_pes(x) \cdot 4}
$$

- 這就是目前 [python/hybridacc_cc/sweep_tools.py](../../../../python/hybridacc_cc/sweep_tools.py#L961) 到 [python/hybridacc_cc/sweep_tools.py](../../../../python/hybridacc_cc/sweep_tools.py#L969) 真正在算的量，也就是現有欄位 `Ideal HW/SW Co-design MACs Utilization (%)` 的實際語意。
- 這個指標適合回答 end-to-end 累積效率。
- 若某個 regime 裡 `MACs(x) = m x`，而 `C_{ideal}(x) = a x + b`，那它必然是有理函數，只會收斂，不會是一條直線。

### 2. 邊際理想利用率

英文：`Marginal Ideal Utilization`

$$
U_{ideal,\Delta}(x_i \rightarrow x_{i+1}) = 100 \cdot \frac{MACs(x_{i+1}) - MACs(x_i)}{\left(C_{ideal}(x_{i+1}) - C_{ideal}(x_i)\right) \cdot active\_pes^{*} \cdot 4}
$$

- 這個指標只應在同一個 `active_pes` regime 內使用；若不同點的 `active_pes` 改變，應先分群再做差分。
- 若同一個 regime 裡 `C_{ideal}(x) = a x + b`，則截距 `b` 會被差分消掉，因此圖上會是水平直線。
- 如果目標是把 steady-state regime 的效率畫成「不再向下收斂」的線，這是最直接的定義。

### 3. 斜率理想利用率

英文：`Slope Ideal Utilization` 或 `Asymptotic Ideal Utilization`

$$
U_{ideal,slope} = 100 \cdot \frac{d MACs / dx}{\left(d C_{ideal} / dx\right) \cdot active\_pes^{*} \cdot 4}
$$

- 這個指標是先把同一個 affine regime 擬合成直線後，再只保留 slope 所得到的 plateau 值。
- 在 fixed-gap regime 裡，它和邊際理想利用率等價；差別只在於一個用鄰點差分，一個用整段擬合。
- 如果報表目的是展示 steady-state scaling efficiency，應優先報這個值，而不是把累積理想利用率誤稱成「理想直線效率」。

### 命名使用準則

- 既有欄位 `Ideal HW/SW Co-design MACs Utilization (%)`，本文一律視為「累積理想利用率」。
- 如果想讓 utilization 圖形呈現 steady-state plateau，應新增「邊際理想利用率」或「斜率理想利用率」，不要直接重命名既有欄位。
- 如果目標是畫出真正的 affine 斜線，應改畫 `C_{ideal}(x)` 或 `last_stop_cycle(x)`，而不是 utilization。

## 分母背後的邊界恆等式

依照目前 simulator 實作：

- boot-up detail 採 inclusive 計數：`cycles = end_cycle - start_cycle + 1`
- drain-out detail 也採 inclusive 計數：`cycles = end_cycle - start_cycle + 1`
- `steady_state_core_cycles` 的定義是：

$$
steady = core\_probe\_cycles\_total - boot\_up\_cycles - drain\_out\_cycles
$$

在目前 log 語意下，`core_probe_cycles_total = drain_out_end_cycle`，而且若 `boot_up_start_cycle = 1`，就有：

$$
boot\_up\_cycles = first\_start\_cycle
$$

$$
drain\_out\_cycles = end\_cycle - last\_stop\_cycle + 1
$$

因此：

$$
steady = end\_cycle - first\_start\_cycle - (end\_cycle - last\_stop\_cycle + 1)
$$

$$
steady = last\_stop\_cycle - first\_start\_cycle - 1
$$

這個恆等式是整件事的核心。

分母裡出現負截距，並不是代表 `IC = 0` 時真的存在某個神祕負成本；它只是因為：

- 先把 `last_stop_cycle` 擬合成 sweep 變數的 affine 線，然後
- 再扣掉固定的 `first_start_cycle`，以及最後那一個不屬於 steady-state 的 STOP 邊界 cycle。

## `last_stop_cycle` 對 workload 呈 affine 成長的關鍵因素

先講結論：對 conv1x1 / conv3x3 這類 fixed-gap regime，`last_stop_cycle` 的 affine 主因不是 simulator，而是「硬體限制固定了 wave 粒度，firmware/runtime 再用固定流程重複執行每一個 wave」。simulator 主要是在做 boundary accounting。

### simulator 的角色：記錄，不是決定

- [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp](../../simulator/include/Core/CoreController.hpp#L1135) 在 retired STOP 時打開 wave-gap window。
- [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp](../../simulator/include/Core/CoreController.hpp#L1158) 在 retired START 時關閉該 window。
- [design/hybridacc-ESL/simulator/include/Core/CoreController.hpp](../../simulator/include/Core/CoreController.hpp#L1218) 在 core halt 後把最後一段整理成 drain-out。
- [design/hybridacc-ESL/simulator/src/main.cpp](../../simulator/src/main.cpp#L846) 只是把整理好的 `last_stop_cycle` 印出來。

換句話說，simulator 決定的是「怎麼量」，不是「為什麼它會線性長大」。

### hardware 的角色：固定 wave 粒度

- conv3x3 lowering 把 `tile_ic` 固定為 `4`，位置在 [python/hybridacc_cc/lowering.py](../../../../python/hybridacc_cc/lowering.py#L589)。
- conv1x1 lowering 把 `tile_ic` 固定為 `12`，位置在 [python/hybridacc_cc/lowering.py](../../../../python/hybridacc_cc/lowering.py#L934)。
- 對應的 total wave 數分別在 [python/hybridacc_cc/lowering.py](../../../../python/hybridacc_cc/lowering.py#L636) 和 [python/hybridacc_cc/lowering.py](../../../../python/hybridacc_cc/lowering.py#L1010) 定義。

因此在 IC sweep 且其他維度固定時，`num_ic_tiles` 會直接隨 IC 線性成長，`total_waves` 也就跟著線性成長。

### firmware/runtime 的角色：重複支付固定 per-wave 成本

- wave 座標推進由 firmware runtime 決定，入口在 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L675)。在 conv/gemm generic path 裡，最內層維度都是 `ic`。
- 每一個 wave 的 runtime 組裝由 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L749) 與 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L844) 準備。
- conv1x1 wave loop 在 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L1704)，conv3x3 在 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L1793)，gemm generic 在 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L2534)。
- 在 conv1x1 / conv3x3 的 fixed regime 裡，每個 wave 都會大致重複同一組流程：`configure -> ensure inputs -> START -> wait done -> STOP -> wait idle`，對應程式可見於 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L1741) 到 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L1770) 與 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L1819) 到 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L1836)。

所以在 conv1x1 / conv3x3 這種 fixed-gap 實驗裡，可以把 affine 因果拆成：

- hardware 決定每個 wave 能吞多少 IC work。
- firmware/runtime 決定要跑多少個 waves，並為每個 wave 重複支付相近的控制、DMA 與 HDDU 執行成本。
- simulator 只是在 STOP/START 邊界上把這些成本量出來。

### 為什麼 gemm 不會維持單一 affine

gemm 的 total wave 數雖然也在 [python/hybridacc_cc/lowering.py](../../../../python/hybridacc_cc/lowering.py#L1409) 以乘積形式定義，但 runtime path 會在 generic、resident full N、resident partial N、single N chunked 之間切換，分流入口在 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../../python/hybridacc_cc/templates/firmware_ops.c.j2#L2534)。

這些 path 會改變：

- 是否重用 PS DMA
- 是否把多個 M/N tile 合併成 resident chunk
- 每一個 wave 內部實際支付的 configure / DMA / wait 成本

因此 gemm 通常只能在局部 regime 內近似 affine，而不會像 conv1x1 / conv3x3 那樣在整個 sweep 上維持單一 affine。

## conv1x1

### Raw 觀察

對當前 raw log 而言，一旦進入 `IC >= 24` 的 multi-wave regime：

- `first_start_cycle = 26451`，每個 case 都一樣
- `drain_out_cycles = 13685`，每個 case 都一樣
- `wave_gap_windows = IC / 12 - 1`
- 每一個 inter-wave gap window 完全固定：
  - `773` cycles
  - `262` instructions
  - `21` MMIO-config instructions
  - `239` data-compute instructions
  - `2` START/STOP control instructions

這代表目前的 conv1x1 sweep 幾乎就是一個標準的 fixed-wave-cost 實驗。

### Raw affine 模型

用目前 raw log 擬合：

$$
last\_stop\_cycle = 428.7916667 \cdot IC + 25606
$$

再代入上面的邊界恆等式：

$$
steady = last\_stop\_cycle - first\_start\_cycle - 1
$$

$$
steady = 428.7916667 \cdot IC - 846
$$

另外：

$$
wave\_gap\_data = 239 \cdot (IC / 12 - 1)
$$

$$
wave\_gap\_data = 19.9166667 \cdot IC - 239
$$

所以目前 raw log 對應的 ideal 分母是：

$$
ideal = steady - wave\_gap\_data
$$

$$
ideal = 408.875 \cdot IC - 607
$$

### `846` 的靜態 firmware 拆解

先講最重要的結論：`846` 不能直接解讀成「boot-up 時段花了 846 cycles」。對這個實際 case 而言，boot-up 到第一個 START 的真實量測是 `26451` cycles、`7038` retired instructions，見 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/sim.log](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/sim.log#L57) 與 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/sim.log](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/sim.log#L61)。

對這個 generated firmware case，靜態設定如下：

- [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c#L33) 到 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c#L36) 顯示 `num_oc_tiles=3`、`num_h_tiles=1`、`num_w_tiles=1`、`num_ic_tiles=2`。
- [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c#L91) 顯示 `conv1x1_resident_oc_tiles=3`，因此這一層只有一個 OC wave group，整層實際上只跑兩個 waves：`ic=0` 與 `ic=1`。
- [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c#L84) 顯示 `parallel_groups=0x0D`，因此第一個 wave 前會真的執行 `cluster_start_layer()`。

把 firmware 靜態攤開後，`846` 比較合理的解讀是：

$$
846 \approx 773 + 73
$$

其中 `773` cycles 是主體，`73` cycles 是較小的殘差項。

### 1. `773` cycles：缺掉的一個 inter-wave gap

對這個 case，raw log 只量到一個 wave gap window，而且它正好就是 `773` cycles，見 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/sim.log](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/sim.log#L63)。

這個數字會出現在 `846` 裡，是因為 affine slope 對每一個新增的 IC tile，實際上都在吸收「一個額外 wave body + 一個額外 gap」的成本；但對一個有 `N` 個 waves 的 layer，真正存在的 gap 數只有 `N-1` 個。也就是說，當我們把 steady-state 擬合成 affine 線後，截距裡天然會先留下「少掉的第一個 gap」。

因此，`846` 裡面大部分不是 boot-up 本身，而是這個結構性缺口：

$$
  ext{negative intercept} = \text{missing first gap} + \text{first-wave asymmetry residual}
$$

在 conv1x1 raw case 中，第一項就是 `773`。

### 2. 剩下的 `73` cycles：第一個 wave 與 steady-state repeated block 不完全同型

把 `773` 拿掉後，還剩約 `73` cycles。這一小段無法只靠靜態程式碼精準切到每一個 cycle，但可以靜態定位出「哪些動作造成第一個 wave 和後續 repeated block 不同」。

第一類是不會隨新增 IC tile 重複支付的 layer bring-up：

- [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L2622) 的 `prepare_layer_common()` 只做一次。
- 其中包含 `set_cluster_mask`、`cluster_set_mode`、`HDDU_CTRL_SOFT_RESET`、四個 AGU bank 的完整設定、HDDU 全域設定、NoC reset/init、scan chain 寫入、PE program patch/load。
- 這個 case 的 payload 長度也能從 generated 檔直接看到：scan chain 長度是 `48`，PE template 長度是 `36`，patch entry 數是 `8`，見 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c#L24) 到 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_data.c#L28)；其內容定義在 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_payload.h](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_payload.h)。
- 光 AGU 設定就有 `4 x 15 = 60` 次 AGU register writes，因為 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_hw.h](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_hw.h#L146) 定義 `AGU_NUM_REGS = 15`，而 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L2622) 會對 PS/PD/PLI/PLO 四個 banks 都呼叫 `cfg_agu_bank()`。

第二類是只發生在第一個 wave 前的 cold-start path：

- [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1688) 的 `run_loop_tiling_conv_1x1()` 在第一個 START 前一定會先做 `dma_init_linear()`，見 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1701)。
- 因為 `parallel_groups != 0`，第一圈還會真的執行 `cluster_start_layer()`，不是 no-op，見 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1722) 到 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1730)。
- 更重要的是，第一圈會走 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1523) 的 `wave_count == 0` 分支，不是後續 wave 的 `prefetch_wait_all()` 分支。

這個 case 的第一個 wave 在 START 前，會同步發出下列 input staging：

- bias load：`dma_load_bias_generic()` 會走 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1478) 到 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1492) 的 `dma_load_parallel_tiles_2d_sync()` 路徑。因為 `conv1x1_resident_oc_tiles=3`，這代表 3 個同步 DMA tile loads。
- PS load：同一個 `wave_count == 0` 分支會走 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1531) 到 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1535) 的 `dma_load_parallel_tiles_sync()` 路徑，同樣是 3 個同步 DMA tile loads。
- PD load：PD plane 不是 parallel path，因此最後會走 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1332) 到 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1394) 的 `dma_load_2d_sync()`，也就是 1 個同步 2D DMA load。

也就是說，第一個 START 之前至少有 `3 + 3 + 1 = 7` 個同步 DMA loads，而後續 wave 則改走 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1190) 的 prefetch path，並在下一圈以 `prefetch_wait_all()` 消化它們。

### 2.5 機器碼佐證

對 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware.elf](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware.elf) 做反組譯後，compiled binary 的控制流和上面的 C code 分析一致，而且足以支持「`73` 來自第一個 wave 的 cold path 殘差」這個說法。

第一，one-time bring-up 在機器碼裡仍然是獨立存在的，不是被編譯器完全折疊掉。

- `prepare_layer_common` 在 ELF 中是一個獨立 symbol，起始位址是 `0x27e8`。
- 反組譯可看到它一開始就直接對 cluster/local MMIO 寫入：先寫 cluster mask，再把 cluster mode 設成 layer-managed，接著發出 HDDU soft reset。
- 同一段機器碼裡還保留了四次 `cfg_agu_bank` 呼叫、一次 `send_noc_scan_chain`、一次 `pe_patch_runtime`，最後以 tail-jump 進 `load_pe_program`。
- 這代表 layer bring-up 的確是 compiled binary 裡的一段真實一次性固定成本，而不是只有在 C code 裡看起來存在。

第二，第一個 wave 和後續 wave 的分流，在機器碼裡也被完整保留下來。

- `ensure_wave_inputs_ready_generic` 在 ELF 中是獨立 symbol，起始位址是 `0x1a3c`。
- 在 `0x1a78` 有 `beqz a3, 0x1a84`。這裡的 `a3` 是 `wave_count`，所以 `wave_count == 0` 時會直接落進 cold path。
- 若 `wave_count != 0` 且 `input_pad_enable == 0`，則控制流會跳到 `0x1c10`，最後在 `0x1c40` tail-jump 到 `prefetch_wait_all`。

也就是說，compiled machine code 直接證明：

- 第一個 wave 走同步 input staging。
- 後續 wave 走等待既有 prefetch 完成的路徑。

第三，第一個 wave 的同步 DMA staging 在機器碼裡可直接看到對應 call site。

- 在 `0x1ae4`，helper 會 `jal 0x11c4 <dma_load_2d_sync>`，而且這個 call 被包在一個 loop 裡，對應 C code 裡的 resident bias 2D DMA load。
- 在 `0x1b48`，helper 會 `jal 0x14a4 <dma_load_sync>`，對應同步 PS load。
- 在 `0x1bfc`，helper 也保留了 `jal 0x14dc <dma_load_parallel_words_sync>` 的分支，代表編譯後仍保有 parallel PS path 的判斷。
- 在 `0x1ba0`，helper 最後以 tail-jump 進 `dma_load_pd_wave_sync`，代表 PD load 也是 cold path 的一部分，而不是在 START 後才補做。

換句話說，第一個 wave 的 compiled helper 不是只做少量 bookkeeping；它確實會在 START 前實際發出多段同步 DMA。

第四，後續 wave 的 prefetch 路徑在機器碼裡不再是一個獨立 helper，而是被 inline 進主 loop。

- 在 ELF symbol table 裡，可以看到 `prefetch_enqueue_linear`、`prefetch_enqueue_2d`、`prefetch_enqueue_parallel_rows`、`prefetch_enqueue_parallel_words`、`prefetch_wait_all`、`prefetch_start`，但看不到獨立的 `prefetch_wave_inputs_conv_1x1` symbol。
- 這表示該 helper 在編譯時被 inline。
- 對應地，在 `run_loop_tiling_conv_1x1` 的機器碼中，`0x2b64` 到 `0x2c34` 這段直接展開了 `prefetch_enqueue_*` 與 `prefetch_start` 的控制流，而不是透過一個獨立函式呼叫。

這個現象和 C code 的語意完全一致：後續 wave 走的是 overlap/prefetch path，而不是第一個 wave 的同步 cold path。

因此，就「閱讀 firmware runtime C code 與機器碼後，能不能佐證 `846` 裡面那個非穩態殘差的來源」這個問題，靜態答案是可以的：

- `773` 來自缺掉的一個第一個 inter-wave gap。
- 剩下的 `73` 並不是某個神祕單一常數，而是 compiled firmware 裡第一個 wave 特有的 cold path 殘差：包含一次性 bring-up、第一次 `cluster_start_layer()`、以及第一個 wave 在 START 前的同步 bias/PS/PD staging。
- 後續 wave 的 compiled path 明顯較 steady-state 化，因為它改走 inline 的 prefetch enqueue + `prefetch_wait_all`。

但同樣要保留邊界：反組譯足以證明「哪些動作只出現在第一個 wave」以及「哪些 helper 在編譯後仍保留為真實控制流」，卻仍不足以把 `73` 精確切分到每一條指令的 cycle。因為真正的 cycle 數還會受到 DMA wait loop、NoC 狀態輪詢與 cluster idle polling 的動態完成時間影響。

### `Steady-state Core Cycles` 與 input channel 的關係

如果只看 raw conv1x1 IC sweep，`Steady-state Core Cycles` 並不是「沒有線性關係」；更精確地說，它是「single-wave 與 multi-wave 兩個不同 regime 的 piecewise 關係」，而在 multi-wave regime 裡它本身就是 affine。

對這個 sweep，令：

$$
N_{ic} = IC / 12
$$

因為 conv1x1 的 `tile_ic = 12`，每多 12 個 input channels，就多一個 IC wave。

### 1. 為什麼單一 wave gap 固定，但 steady-state 不是同一條線

原因是 `Steady-state Core Cycles` 不只包含 gap；它還包含每個 wave 內部真正的 non-gap 執行時間。因此它的分解應該寫成：

$$
C_{steady}(IC) = C_{body,non-gap}(IC) + C_{gap,total}(IC)
$$

其中，對目前 raw conv1x1 multi-wave 資料：

$$
C_{gap,total}(IC) = 773 \cdot (IC/12 - 1) = 64.4167 \cdot IC - 773
$$

而把 gap 全部扣掉之後，剩下的 non-gap steady work 也是 affine：

$$
C_{body,non-gap}(IC) = C_{steady}(IC) - C_{gap,total}(IC)
$$

$$
C_{body,non-gap}(IC) = 364.375 \cdot IC - 73
$$

因此：

$$
C_{steady}(IC) = (364.375 + 64.4167) \cdot IC - (73 + 773)
$$

$$
C_{steady}(IC) = 428.7917 \cdot IC - 846
$$

這直接回答了這個觀察：

- 單一 wave gap 固定，代表的是 `C_{gap,total}` 的 slope 固定。
- 但 `Steady-state Core Cycles` 的 slope 是「gap slope + non-gap wave body slope」，所以它當然不是只跟 gap 一樣的那條線。
- 在目前 conv1x1 raw sweep 裡，gap slope 只有 `64.4167 cycles / IC`，而 non-gap body slope 是 `364.375 cycles / IC`；也就是說 steady-state 的主要成分其實是每個 wave 的有效執行時間，不是 gap。

### 2. 為什麼視覺上會覺得 steady-state 沒那麼線性

主要有兩個原因。

第一，`IC = 12` 是 single-wave regime，沒有任何 inter-wave gap。

- `IC = 12` 時，raw steady-state 是 `4384` cycles。
- `IC >= 24` 才進入 multi-wave regime，從這一點開始才會出現固定的 `773`-cycle inter-wave gap。

因此如果把 `IC = 12` 和 `IC >= 24` 的點全部畫在同一條線上看，會看到一個 regime 轉折；這不是 steady-state 不線性，而是你把 single-wave 與 multi-wave 混在一起看。

第二，即使在 multi-wave regime 裡，steady-state 也帶固定負截距 `-846`，所以 `steady / IC` 不會一開始就固定，而是會往 slope 收斂。

目前 raw 資料可以直接看出這件事：

| IC | Raw steady-state cycles | Raw gap total | Raw non-gap body | steady / IC |
| --- | ---: | ---: | ---: | ---: |
| 12 | 4384 | 0 | 4384 | 365.3333 |
| 24 | 9445 | 773 | 8672 | 393.5417 |
| 48 | 19736 | 2319 | 17417 | 411.1667 |
| 96 | 40318 | 5411 | 34907 | 419.9792 |
| 192 | 81482 | 11595 | 69887 | 424.3854 |
| 384 | 163810 | 23963 | 139847 | 426.5885 |
| 768 | 328466 | 48699 | 279767 | 427.6901 |

可以看到：

- `steady / IC` 會往 `428.7917` 收斂。
- `gap_total / IC` 會往 `64.4167` 收斂。
- `non-gap body / IC` 會往 `364.375` 收斂。

所以如果你看的是「每 IC 的平均 steady-state cost」而不是 `Steady-state Core Cycles` 對 `IC` 的散點，視覺上就會覺得它不是一條完全固定斜率的線；其實那只是固定負截距造成的收斂現象。

### 3. 對這個問題最精確的結論

- 在 conv1x1 raw multi-wave regime 中，`Steady-state Core Cycles` 對 `IC` 是 affine，不是任意非線性。
- 單一 wave gap 固定，只能說明 `gap_total` 的 slope 固定；它不能決定 `steady-state` 的整體 slope，因為 steady-state 還包含每個 wave 的 non-gap body。
- 若把 `IC = 12` 的 single-wave 點也放進來，整體關係應描述成 piecewise：`IC=12` 是 single-wave 點，`IC>=24` 才是 multi-wave affine 線。
- 若使用 stale report CSV，steady-state 還可能被額外平移一個 gap window，進一步讓圖看起來更不像 raw affine 關係；因此這裡仍應以 raw log 為準。

### 3. 哪些動作不應該算進 `846`

- 最後一個 wave 的 writeback 在 [output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c](../../../../output/analysis-wave-gap/conv1x1_ic_sweeps/conv1x1_ic_oh16_ow64_ic24_oc48/firmware_ops.c#L1757) 發生，但它是在 STOP 之後才執行，因此會落到 drain-out，不屬於 steady-state 的 `846`。
- 因此，`846` 的靜態 root cause 應該描述成「一個缺掉的第一個 inter-wave gap，加上第一個 wave 與 steady-state repeated block 的非穩態殘差」，而不是單純稱為 boot-up cycles。

### 4. 靜態分析的邊界

靜態程式碼可以明確指出 `846` 對應的動作集合，但無法只靠 source code 把剩下的 `73` cycles 一一分派到單條指令。原因是這些 helper 內部大量包含 DMA/NOC/cluster status polling，而其 cycle cost 取決於實際硬體就緒時間。也就是說，source code 足以定位 root cause，但若要把 `73` 再切成精確 cycle bucket，還需要更細的動態 trace。

### 為什麼 ideal utilization 不是固定值

對 conv1x1：

$$
MACs = 49152 \cdot IC
$$

$$
U_{ideal} = 100 \cdot \frac{49152 \cdot IC}{48 \cdot 4 \cdot (408.875 \cdot IC - 607)}
$$

這不是常數比，而是一個會漸近收斂的有理函數。

目前 raw log 算出的 ideal utilization 是：

| IC | Raw ideal cycles | Raw ideal utilization (%) |
| --- | ---: | ---: |
| 24 | 9206 | 66.7391 |
| 48 | 19019 | 64.6091 |
| 96 | 38645 | 63.5943 |
| 192 | 77897 | 63.0987 |
| 384 | 156401 | 62.8538 |
| 768 | 313409 | 62.7321 |

它會往下收斂，原因就是固定的 `-607` 在 IC 越大時影響越小。

### 使用者看到的 `-73` 是從哪裡來的

`-73` 不是目前 simulator 真實邏輯下的截距。

它來自既有 conv1x1 report CSV。那份 report 對 multi-wave case 多算了一整個 gap window：

$$
steady_{csv} = steady_{raw} + 773
$$

而 raw 模型本來是：

$$
steady_{raw} = 428.7916667 \cdot IC - 846
$$

所以 CSV 版本就變成：

$$
steady_{csv} = 428.7916667 \cdot IC - 73
$$

因為：

$$
-846 + 773 = -73
$$

也就是說，使用者看到的 `-73` 是現有 report 產物的 artifact，不是目前 simulator profiling 邏輯本身的常數。

## conv3x3

### Raw 觀察

conv3x3 和 conv1x1 屬於同一類現象。

對目前 raw log 而言，一旦進入 `IC >= 8` 的 multi-wave regime：

- `first_start_cycle = 23395`
- `drain_out_cycles = 11455`
- `wave_gap_windows = IC / 4 - 1`
- 每一個 inter-wave gap window 都固定為：
  - `645` cycles
  - `209` instructions
  - `21` MMIO-config instructions
  - `186` data-compute instructions
  - `2` START/STOP control instructions

### Raw affine 模型

最後一個 STOP cycle 可以精確寫成：

$$
last\_stop\_cycle = 2986 \cdot IC + 22669
$$

因此：

$$
steady = 2986 \cdot IC - 727
$$

再加上：

$$
wave\_gap\_data = 186 \cdot (IC / 4 - 1) = 46.5 \cdot IC - 186
$$

所以 raw ideal 分母是：

$$
ideal = 2939.5 \cdot IC - 541
$$

### Utilization 行為

對 conv3x3：

$$
MACs = 387072 \cdot IC
$$

目前 raw log 算出的 ideal utilization：

| IC | Raw ideal cycles | Raw ideal utilization (%) |
| --- | ---: | ---: |
| 8 | 22975 | 80.2263 |
| 16 | 46491 | 79.2928 |
| 32 | 93523 | 78.8341 |
| 64 | 187587 | 78.6067 |
| 128 | 375715 | 78.4935 |
| 256 | 751971 | 78.4371 |
| 512 | 1504483 | 78.4089 |

所以 conv3x3 的故事和 conv1x1 相同：

- 固定的 per-gap 成本
- 固定的 first-start 邊界
- affine 形式的分母
- utilization 只會收斂，不會從頭到尾完全固定

既有 conv3x3 summary CSV 也有類似偏差，對 multi-wave case 會多出一個 gap window (`645`)，因此也應該以 raw log 為準。

## gemm

### 高層結論

gemm 只算部分相同。

它在單一 case 上，仍然服從同樣的邊界恆等式：

$$
steady = last\_stop\_cycle - first\_start\_cycle - 1
$$

但它不像 conv1x1 / conv3x3 那樣，整個 sweep 都落在一條乾淨的 affine 線上。

### gemm 不同的地方

1. `first_start_cycle` 不是全域固定。

從 raw log 可看到：

- `(m=48, n=256)` 和 `(m=48, n=512)` 都是 `first_start_cycle = 10639`
- `(m=96, n=256)` 和 `(m=96, n=512)` 都是 `first_start_cycle = 13205`
- 但 `(m=192, n=256)` 是 `18323`，`(m=192, n=512)` 卻是 `13347`
- `(m=384, n=256)` 是 `18465`，`(m=384, n=512)` 卻是 `13347`

2. gap window 成本不固定。

從 raw `sim.log` 的 window detail 可看到代表性例子：

- `m=48, n=256`：gap cycles 可能是 `299`、`495`、`1237`
- `m=192, n=512`：gap cycles 可能是 `315`、`1631`、`9758`
- `m=768, n=128`：gap cycles 可能是 `2485`、`29746`

3. 因為 start boundary 和每個 gap 的成本都會隨 regime 改變，所以整個 gemm_mn sweep 不存在單一 conv1x1 式的收斂常數。

### 但 gemm 在局部 regime 內仍然是 affine

如果只看固定 regime，gemm 仍然可以寫成 affine 關係。

例如：

- 對 `m = 48`，只看 multi-wave 點 `n = 256, 512`：

$$
steady = 49.203125 \cdot n + 385
$$

$$
ideal = 47.5625 \cdot n + 415
$$

- 對 `m = 96`，只看 multi-wave 點 `n = 256, 512`：

$$
steady = 75.328125 \cdot n + 1111
$$

$$
ideal = 74.140625 \cdot n + 997
$$

但這些係數在不同 `m` 下不穩定，所以 gemm 只能說是 piecewise affine，不能說是全域 affine。

### gemm 的結論

- 在公式層面：相同
- 在像 conv1x1 / conv3x3 那樣的單一固定 gap regime：不同

真正原因不是 utilization 公式本身，而是 gemm runtime 在不同 sweep 點下，tiling / wave 結構會變，所以 boundary 常數和 gap 組成都不再固定。

## 最後總結

### 1. conv3x3 / gemm 和 conv1x1 是不是同一種情況？

- conv1x1：是，屬於乾淨的 fixed-gap regime
- conv3x3：是，和 conv1x1 同型
- gemm：只有局部同型，整個 sweep 不是單一 affine regime

### 2. `-73` 到底從哪裡來？

- 它不是目前 simulator 邊界定義本身產生的常數。
- 目前 raw log 下，conv1x1 真正的關係是：

$$
steady_{raw} = 428.7916667 \cdot IC - 846
$$

- 使用者看到的 `-73`，只出現在既有 conv1x1 summary CSV，因為那份 report 對 multi-wave case 多算了一整個 conv1x1 gap window：

$$
steady_{csv} = steady_{raw} + 773 = 428.7916667 \cdot IC - 73
$$

- 所以 `-73` 是 report artifact，不是目前 simulator denominator logic 的真常數。

### 3. 那目前真正的負截距來源是什麼？

對目前 source / raw log，真正的來源就是 steady-state 的邊界定義：

$$
steady = last\_stop\_cycle - first\_start\_cycle - 1
$$

只要 `last_stop_cycle` 對 workload 呈 affine 成長，而 `first_start_cycle` 固定，分母在擬合後就必然帶一個固定截距。這個截距本質上只是「工作量相關的執行時間線」，在扣掉固定 startup boundary 與最後 STOP 邊界 cycle 之後的線性外插結果。

對 conv1x1 raw multi-wave sweep 而言，若進一步攤開這個固定截距，靜態 firmware 分析顯示它最合理的拆法是：

$$
846 \approx 773 + 73
$$

其中 `773` 是缺掉的第一個 inter-wave gap，`73` 則是第一個 wave 與 steady-state repeated block 之間的非穩態殘差，而不是另一個獨立的 boot-up phase 常數。

### 4. 如果報表要把 utilization 定義固定下來，應該怎麼做？

- 既有 `Ideal HW/SW Co-design MACs Utilization (%)` 應明確改讀成「累積理想利用率」，不要再把它當成 plateau 指標。
- 若目標是讓圖呈現 steady-state 的水平直線，應新增「邊際理想利用率」或「斜率理想利用率」。
- 若目標是展示 affine 關係本身，應改畫 `ideal core cycles` 或 `last_stop_cycle` 對 workload 的圖，而不是 utilization。