# cluster_gen.py 編譯邏輯教學說明

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

本文是 `python/hybridacc_verify/gen/cluster_gen.py` 的完整教學導覽，目標是讓你從「輸入 config」一路理解到「產出 test data / scan chain / DMA / SPM / cluster plan」。

---

## 1. 檔案定位與責任

`cluster_gen.py` 的核心責任有三層：

1. 建立測試資料（activation / weight / partial_sum / golden output）。
2. 建立硬體控制資料（scan chain + PE program）。
3. 建立執行期記憶體與搬運規格（SPM section + DMA waves + AGU cluster_plans）。

對外主要入口：

- `generate_conv2d_test(...)`
- `generate_gemm_test(...)`

---

## 2. 先備概念：資料打包與位址單位

### 2.1 64-bit word 與 fp16

NoC 以 64-bit 為主要搬運粒度，一個 word 可裝 4 個 fp16。

- `_num_words64_from_shape(shape)`：
  - `elems = prod(shape)`
  - `words64 = ceil(elems / 4)`

### 2.2 local / global / linear / parallel

SPM 位址有兩組概念：

1. **global SPM byte address**：DMA descriptor 使用。
2. **local SPM word address**：AGU runtime base 使用。

`_to_group_local_word_addr(addr_bytes)` 會把 byte address 映射回 group local word address，供 AGU 寫入 `base_addr`。

---

## 3. 波次切分工具

- `_get_wave_range(total, waves, wave_idx)`：平均切塊。

`_get_wave_range` 是 Conv2D 的時域 wave 排程基礎；GEMM 的 wave 排程沿用 NoC GEMM 測試的規劃（見 §7）。

---

## 4. AGU template 與 cluster plan 生成

### 4.1 AGU 欄位模板

- `_new_agu_cfg(enable=False, ultra=False)` 產生統一格式 AGU dict：
  - `base_addr`, `iter0..3`, `stride0..3`, `tag_base`, `tag_stride*`, `tag_ctrl`, `mask_cfg`, `ultra`, `enable`。

### 4.2 Conv2D 計畫：`_compile_cluster_plans_conv2d(...)`

流程摘要：

1. 依 `wave_schedule` 的 `(wh, woc, wic)` 遍歷每個 wave。
2. 由 `runtime_addr_per_wave` 取當前 wave 的 tensor base（weight/activation/partial_sum/output）。
3. 產生 `agu_ps/pd/pli/plo` 的 iter/stride/tag。
4. 可透過 `agu_ultra_overrides` 針對個別 AGU 強制改 `ultra`。

這個 API 主要服務 conv path，也提供 GEMM 可借用的結構範式。

### 4.3 GEMM 計畫：`_compile_cluster_plans_gemm(layout, dram_mapping)`

契約（與 cc 的 GEMM lowering / firmware 相同；testbench 整層只送一次 START_PE，並以 `plans[i]` 配 `dma.waves[i]`）：

1. 每個時域 wave 產生**一個** cluster plan 與**一個** DMA wave，順序為 N wave 在外、M wave 在內，與 PE 程式的迴圈一致（每個 N wave 一次 `SWAPDM`，每個 M wave 一次 `LDMA.ACT`）。
2. 每個 plan 以 wave 內編號餵該 wave 的全部 `(m, n)` PE tile，與 scan chain 一致：
	- PS：`iter=[2, 32, grid_n_per_wave, 1]`，tag = N tile（`tag_ctrl=2`）。
	- PD：`iter=[3, grid_m_per_wave, 32, 1]`，tag = M tile（`tag_ctrl=1`）。
	- PLI/PLO：`iter=[3, 8, grid_m_per_wave*grid_n_per_wave, 1]`，tag = `m*grid_n_per_wave+n`（`tag_ctrl=2`）。
3. PS 只在每個 N wave 的第一個 M wave 送出（其餘 plan 的 `global_mask=0xE`，PS AGU `enable=false`），因為 PE 每個 N wave 只收一組 weight。
4. bus b 是 K stage b：PS/PD 群組的 bank b 放 K stage b，AGU 讀 parallel 區（ultra），port b 餵 bus b；PLI/PLO 讀寫 linear 區（non-ultra）。
5. PD/PLI/PLO 依 wave 交替 ping/pong，PS 依 N wave 交替。

### 4.4 GEMM 的 AGU ultra 設定

PS/PD 為 ultra（一次讀三個 bank、各 port 一個 K stage）；PLI/PLO 為 non-ultra：PLI 只進第一個 K stage（bus 0），PLO 只從最後一個 K stage 讀回，與 `test_noc_sim` 在 ultra + K-split 時的行為一致。`meta["agu_ultra_overrides"]` 記錄為 `{"agu_pli": False, "agu_plo": False}`。

---

## 5. DMA/SPM 規劃器：`_build_spm_dma_plan(...)`

這是整份檔案最關鍵的中樞，輸出：

- `spm`: group/section/tensor_mapping
- `dma`: waves/transfers/spm_map

### 5.1 拓樸模型

預設參數：

- `num_groups = 4`（PS/PD/PLI/PLO）
- `banks_per_group = 3`
- `bank_depth_words = 8192`

每 group 同時有：

1. linear 區（連續地址，較適合一般 DMA）
2. parallel 區（面向並行 PE 取數）

### 5.2 section_mode 與 spm_mode

每個 tensor 有兩個獨立政策：

1. `section_mode`
	- `group`: 使用 `gX_ping/pong`
	- `bank`: 使用 `gX_bY_ping/pong`
2. `spm_mode`
	- `linear`: runtime base 用 `local_linear_base`
	- `parallel`: runtime base 用 `local_parallel_base`

優先順序：

1. 使用者顯式傳入 `tensor_section_mode` / `tensor_spm_mode`。
2. 否則採預設推斷。

### 5.3 wave transfer 生成

每個 wave 會生成：

- `spm_map`：PS/PD/PLI/PLO 對應到哪個 group。
- `runtime_sections`：本 wave 各 tensor 實際 section。
- `transfers`：DMA descriptor。

`transfer` 會包含：

- `direction` (`dram_to_spm` / `spm_to_dram`)
- source/destination address
- `size_words64`
- 選配 `src_addr_gen` / `dst_addr_gen` / slice metadata

### 5.4 invariant 檢查

`assert_wave_transfer_invariants(...)` 會驗證 partial_sum/output 的 group 與 section 是否符合當下 `spm_map`，避免 map 交換後寫錯區域。

---

## 6. Conv2D 入口：`generate_conv2d_test(...)`

流程順序：

1. 讀 config，產生隨機輸入。
2. 視 kernel 需要可分段（如 `k5` 拆成 `3+2`）。
3. 跑 golden conv。
4. 建 scan chain（含 route mode）。
5. 組 `software_config`。
6. 呼叫 `_build_spm_dma_plan(...)` 產生 SPM+DMA。
7. 呼叫 `_compile_cluster_plans_conv2d(...)` 產生 AGU 計畫。
8. 回傳 `ClusterTestData`。

---

## 7. GEMM 入口：`generate_gemm_test(...)`

流程順序：

1. `plan_cluster_gemm(config, config.pe_program)`（不需 torch）：
	- 以 `noc_gen.plan_gemm_test` 取得 wave 規劃（`PE_M=12, PE_N=8, PE_K=32`）、wave 內編號的 scan chain，並檢查 fixture PE 程式的 N/M wave 迴圈與 `SDMA.LOOP` 是否符合規劃（不符即 `ValueError`）。
	- 只支援 ultra K-chain 且只有一個 K wave（`1 < grid_k <= num_bus`）；wave 大小不一、non-ultra、`grid_k == 1` 或需要多個 K wave 時直接 `ValueError`，不產生不一致的測試。
	- 產生 SPM sections、DMA waves 與 cluster plans（§4.3）。
2. 產生 A/B/D 與 golden C（seed 與 NoC GEMM 測試相同）。
3. `pack_cluster_gemm_tensors(...)` 轉成 §8.1 的 packed DRAM 影像（golden C 也同樣打包）。
4. 回傳 `ClusterTestData`。

---

## 8. GEMM addressing policy

只產生 ultra K-split（`grid_k > 1`）：

- `weight/activation`
  - `section_mode = bank`
  - `spm_mode = parallel`
- `partial_sum/output`
  - `section_mode = group`
  - `spm_mode = linear`

原因：

- PS/PD 在 ultra 為多 port 並行資料打包，適合 parallel。
- K-split 時 PLI/PLO 走單路累加/讀回語意（尤其 PLO 為 standard read），適合 linear。

### 8.1 packed DRAM 影像

DMA 只做連續複製，所以 `input_*.bin` 與 `output_partial_sum.bin` 直接存成 AGU 讀取的 packed wave tile（不是 row-major 矩陣；`meta["dram_layout"]` 記錄格式）：

- `activation`：每個 M wave 一塊 `A[m_wave, :K]^T`，K 為外層、每個 64-bit word 放 4 列（PE 的 PD 封包是 4 個 M 值）。每塊依 K stage 切成 bank 大小的三段，各自 DMA 到 `g1_b{0,1,2}`；`M=48, K=96` 時每 bank 384 words。
- `weight`：每個 N wave 一塊 `B[:K, n_wave]`，K 為外層、每 word 4 行；同樣依 K stage 分到 `g0_b{0,1,2}`。
- `partial_sum` / `output`：每個 wave（N 外、M 內）一塊；塊內依 PE tile `(m, n)`，每個 tile 8 行、每行 12 列（3 words）。

M、N、K 不是 tile 倍數時以 0 補齊，golden 也一樣補 0。

---

## 9. GEMM AGU ultra policy

見 §4.4：PS/PD ultra，PLI/PLO non-ultra。

---

## 10. 產物結構快速索引

### 10.1 `software_config["spm"]`

- `topology`
- `groups`
- `tensor_mapping`

### 10.2 `software_config["dma"]`

- `waves[i].spm_map`
- `waves[i].runtime_sections`
- `waves[i].transfers[*]`

### 10.3 `software_config["cluster_plans"]`

每個 plan 含：

- `name`, `ultra_mode`, `global_mask`
- `agu_ps/pd/pli/plo`

---

## 11. 實務除錯建議

1. 先看 `tensor_mapping`：
	- 確認每個 tensor 的 `section_mode` 與 `spm_mode` 是否符合預期。
2. 再看 `dma.waves[*].runtime_sections`：
	- 確認 ping/pong 交替與 group mapping 是否正確。
3. 最後看 `cluster_plans[*].agu_*`：
	- `base_addr` 是否在同一 addressing space（local word）
	- `iter/stride` 是否與打包維度一致。
4. 若是 prefetch overlap 問題：
	- DMA 為 global byte address；AGU footprint 為 local word address。
	- 比較前必須先轉成同一空間。

---

## 12. 小結

`cluster_gen.py` 的關鍵不只是「產生資料」，而是把三件事同時對齊：

1. TestBench 封包/tag 行為
2. SPM 區域與 DMA 搬運策略
3. Cluster AGU 的 base/iter/stride/tag 規格

GEMM 只產生 ultra K-split 一種模式，plan、DMA wave、scan chain、PE 程式迴圈與 packed 資料由同一份 wave 規劃決定；其他模式會直接報錯。
