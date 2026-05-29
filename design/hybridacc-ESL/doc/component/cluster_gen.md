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

### 3.1 連續切分

- `_get_wave_range(total, waves, wave_idx)`：平均切塊。

### 3.2 tile-aware 切分

- `_get_wave_tile_range(tiles_per_wave, wave_idx, total_tiles, fallback_waves)`：
  - 若有明確 `grid_*_per_wave`，照指定 tile 數切。
  - 否則回退到平均切分。

這兩個函式是 Conv2D/GEMM 共同的時域 wave 排程基礎。

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

### 4.3 GEMM 計畫：`_compile_cluster_plans_gemm(...)`

流程摘要：

1. 以 `waves_k * waves_n * waves_m` 遍歷 wave。
2. 每個 wave 再遍歷 `kt`（K tile）與 `mt`（M tile）。
3. 依矩陣打包模型計算：
	- `row_w = ceil(N/4)`
	- `col_d = ceil(M/4)`
	- `tile_w = ceil(pe_n/4)`
	- `tile_d = ceil(pe_m/4)`
4. 為 `agu_ps/pd/pli/plo` 產生 base/iter/stride/tag。
5. `wk == 0` 才啟用 `agu_pli`；`wk == last` 才啟用 `agu_plo`。

### 4.4 GEMM 新增：AGU ultra 覆寫

目前 `_compile_cluster_plans_gemm` 支援 `meta["agu_ultra_overrides"]`，可針對個別 AGU 覆寫 ultra bit，例如：

- `{"agu_plo": False}`

用途：對齊 `test_noc_sim` 在 **ultra + K-split** 時，PLO 使用標準（non-ultra）read request 的行為。

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

1. 計算 `grid_m/grid_n/grid_k`（以 `PE_M=12, PE_N=8, PE_K=32`）。
2. 推導 `wave_m/wave_n/wave_k` 與 `grid_*_per_wave`。
3. 產生 A/B/D 與 golden C。
4. 依 K-split 拓樸生成 scan chain：
	- ultra: `ps_id=n_idx`, `pd_id=m_idx`
	- normal: `ps_id=k_idx*grid_n+n_idx`, `pd_id=k_idx*grid_m+m_idx`
5. 建立 `software_config` 與 `tensor_words64`。
6. 呼叫 `_build_spm_dma_plan(...)`。
7. 呼叫 `_compile_cluster_plans_gemm(...)`。
8. 回傳 `ClusterTestData`。

---

## 8. GEMM addressing policy（本次更新重點）

為對齊 `test_noc_sim` 的實際傳輸行為，`generate_gemm_test(...)` 已加入明確 policy：

### 8.1 non-ultra

- `weight/activation/partial_sum/output`
  - `section_mode = group`
  - `spm_mode = linear`

### 8.2 ultra 且 `grid_k == 1`

- `weight/activation/partial_sum/output`
  - `section_mode = bank`
  - `spm_mode = parallel`

### 8.3 ultra 且 `grid_k > 1`（K-split）

- `weight/activation`
  - `section_mode = bank`
  - `spm_mode = parallel`
- `partial_sum/output`
  - `section_mode = group`
  - `spm_mode = linear`

原因：

- PS/PD 在 ultra 為多 port 並行資料打包，適合 parallel。
- K-split 時 PLI/PLO 走單路累加/讀回語意（尤其 PLO 為 standard read），適合 linear。

### 8.4 GEMM DMA slicing 改為 packed 4D + K 軸切分

為了避免 activation 在 ultra bank partition 時因 shape words 檢查失敗而 fallback，GEMM 傳入 `_build_spm_dma_plan(...)` 的 `tensor_shapes` 已改成 packed-friendly 4D 形式：

- `activation`: `[1, M, 1, K*4]`
- `weight`: `[1, K, 1, N*4]`
- `partial_sum`: `[1, M, 1, N*4]`
- `output`: `[1, M, 1, N*4]`

此表示法下，`dim3_words = ceil(dim3_elems/4)` 正好等於 packed word 維度，因此可與 `tensor_words64` 對齊。

另外，bank-mode 的 activation 在 GEMM layout（`[1, M, 1, K*4]`）下，partition 不再沿 height 分，而是沿 packed channel（K 軸）分。以 `M=48, K=96`、3 banks 為例：

- activation 總 words = `48*96/4 = 1152`
- 每 bank words = `48*32/4 = 384`

這樣 DMA 會生成 3 筆 activation transfer，與 K-split 的資料語意一致。

---

## 9. GEMM AGU ultra policy（本次更新重點）

在 `meta_config` 中新增 `agu_ultra_overrides`：

- 當 `ultra_mode=True` 且 `grid_k>1` 時，設定 `{"agu_plo": False}`。

用途：讓 PLO AGU 與 testbench 的 non-ultra read path 一致。

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

本次 GEMM 更新的核心價值是把 addressing policy 與 AGU ultra 行為做成顯式規則，讓 non-ultra、ultra 單 K、ultra K-split 三種模式的語意一致且可維護。
