單一 PE 的計算可以支援以下的運算

1. Conv1D, kernel size=1, input channel=12, output channel(kernel number)=16, stride=1, padding=0, input feature map width is not limited
2. Conv1D, kernel size=3, input channel=4, output channel(kernel number)=16, stride=1, padding=1, input feature map width is not limited
3. Conv1D, kernel size=5, input channel=2, output channel(kernel number)=16, stride=1, padding=2, input feature map width is not limited
4. Conv1D, kernel size=7, input channel=1, output channel(kernel number)=16, stride=1, padding=3, input feature map width is not limited

5. Gemm MxK * KxN = MxN, M = 8, N = 12, K = 32, partial sum is supported

PE Array 的組成
單一 PE Array 由 4x7 的 PE 組成, 共 28 個 PE。
PE 可以共用 input feature map 的資料, 但不共用 weight 資料，weight 資料需要在每個 PE 中各自儲存一份。
上下之間的 7個 PE 可以傳遞 partial sum, 左右之間的 4個 PE 不可以傳遞 partial sum。

PE Cluster 的組成
PE Cluster 由 6 個 PE Array 組成, 共 168 個 PE。
共有 ６ 個 Global Buffer, 每個 Global Buffer 連接一個 PE Array。

---
需要一個演算法與工具將現有 Conv2D 與 Gemm 的工作負載轉換成上述 PE Array 可以支援的工作負載。
主要是能產出 tiling 的參數, 以及 mapping 的參數。並且將 tiling 與 mapping 的參數帶入 analytical model 中進行分析。生成 latency 與 energy 的估算。
另外產生對應的 schedule IR code, 以及對應的 hardware configuration file。
---

// ============================================================================
// 設計構想 (Design Overview)
// ============================================================================

[核心觀察]
單一 PE 支援的四種 Conv1D pattern 其 (kernel_size * in_channels) = 12 (統一運算槽寬度 KcFold = 12)。
單一 PE 亦固定輸出通道同時處理 16 個 (OFold = 16)。
=> 將任意 Conv2D 之 (Cin * Kh * Kw) 線性化後切片為長度 12 的 micro-kernel 片段；每片映射到一種合法 (k, c) pattern (1x12 / 3x4 / 5x2 / 7x1)，不足則以 padding + weight mask 處理。

[目標]
1. Conv2D / GEMM → 以 (KcFold=12, OFold=16) 為基本計算原子進行分塊。
2. 建立多層次 tiling：Cluster / Array / PE Column / PE Row / Temporal Waves。
3. 建立 mapping：loop 維度 → (空間並行 + 時間分批 + 部分和傳遞)。
4. 產出:
   - tiling_parameters.json
   - schedule_ir.json
   - hw_config.yaml
   - analysis_report.json (latency & energy)

[輸入工作負載格式範例]
Conv2D:
{
  "op": "conv2d",
  "N": batch,
  "Cin": C_in,
  "Cout": C_out,
  "Hin": H_in,
  "Win": W_in,
  "Kh": K_h,
  "Kw": K_w,
  "stride_h": S_h,
  "stride_w": S_w,
  "pad_h": P_h,
  "pad_w": P_w,
  "dilation_h": D_h,
  "dilation_w": D_w,
  "data_type": "int8|fp16|..."
}

GEMM:
{
  "op": "gemm",
  "M": M,
  "N": N,
  "K": K,
  "layoutA": "row-major",
  "layoutB": "col-major"
}

[輸出核心 tiling 參數 (概念)]
{
  "Cin_fold": 12,               // reduction micro-tile size
  "Cout_fold": 16,              // output channel micro-tile
  "num_reduction_slices": ceil(Cin*Kh*Kw / 12),
  "Cout_tiles": ceil(Cout / 16),
  "spatial_tiling": {
     "H_outer": ..., "H_inner": ...,
     "W_outer": ..., "W_inner": ...
  },
  "mapping": {
     "cluster": {...},
     "array_id": "...",
     "pe_row_dim": "partial_sum_depth",
     "pe_col_dim": "parallel_output_channel_or_spatial",
     "temporal_wave": {...}
  }
}

[Conv2D -> 1D micro-kernel 分解演算法]
1. FlattenFilter = Cin * Kh * Kw
2. R = FlattenFilter
3. For slice_id in 0..ceil(R/12)-1:
     take 12 elements (或最後不足以 0 padding)
     選擇 pattern:
       優先使用 k 等於實際空間 kernel 尺寸切分序列 (啟用連續元素)：
         嘗試 k in {7,5,3,1}，若 slice 中連續度不足則降級
       對應 in_channels = 12 / k
     產生 weight_block (k, in_channels=12/k)
4. 對應特徵圖輸入：對每個輸出位置產生 k 長度的 sliding segment；in_channels group 採 channel blocking。
5. Accumulate 所有 slice 的部分和 → 最終輸出。

[Partial Sum 傳遞策略]
- 7 個垂直 PE: pipeline 方式串接 slice accumulation。
- 若 num_reduction_slices > 7:
    分批 (waves)，每批最多 7 slice。
    每批完成後將結果寫回 Global Buffer，再讀出與下一批結果相加，或在 SRAM 中多 buffer (double-buffer PS)。

[PE Array Mapping 策略 (Heuristic)]
- 4 (水平) × 7 (垂直)
  垂直: reduction slice accumulation (partial sum chain)
  水平: 4 個並行任務 (可為 output channel tiles 或空間 tiles)
  規則:
    若 Cout_tiles >= 4: 水平放 4 個不同 Cout tile
    否則 用空間 W 分段填滿
    若仍不足 → 混合 H_out strip
- 6 Arrays:
  依優先順序分配：
    1. 不同 batch N
    2. 不同 Cout tile group (外層)
    3. 大型空間區塊 (H_outer/W_outer)
  規模較小時可選擇 power gating 不使用全部 Array。

[GEMM 映射到 (8,12,32) 基本形]
- 基本 PE 能力固定 (假設): M_fold=8, N_fold=12, K_fold=32
- 將 M,N,K 分別做: ceil(M/8), ceil(N/12), ceil(K/32)
- K 維度切片對應 vertical reduction chain (與 Conv 類似)
- N 或 M 映射到水平方向；另一維度擴散到多 Array/Cluster
- 若 K slice > 7 需 temporal waves

[Schedule IR (範例片段)]
pseudo (JSON 形式):
{
 "loops": [
   {"name":"n","range":N,"tile":[N_outer,N_inner]},
   {"name":"co","range":Cout,"tile":[Cout_outer,16]},
   {"name":"ho","range":H_out,"tile":[H_outer,H_inner]},
   {"name":"wo","range":W_out,"tile":[W_outer,W_inner]},
   {"name":"red","range":Cin*Kh*Kw,"tile":[Red_outer=12]},
 ],
 "mapping": {
   "cluster_level": ["n.outer","co.outer"],
   "array_level": ["ho.outer","wo.outer"],
   "pe_col": ["co.inner OR wo.inner (policy)"],
   "pe_row": ["red.outer (accumulate chain)"],
   "temporal_waves": ["red.wave", "co.wave (if needed)"]
 },
 "compute": [
   {"op":"mac","tensor":"O_partial","reduce_over":"red.outer"},
   {"op":"accumulate","chain_length":7,"activation":"ReLU","fuse_position":"post_wave"}
 ],
 "buffers": {
   "global_ifm":"shared_across_array",
   "global_ofm":"per_array_partition",
   "weights":"per_pe_replicated"
 },
 "notes":"last red tile uses mask if padded"
}

[Hardware Config (hw_config.yaml 範例)]
hardware:
  clusters: 1
  arrays_per_cluster: 6
  array_shape: [rows:7, cols:4]
  pe:
    conv1d_patterns:
      - {k:1, in_channels:12, out_channels:16}
      - {k:3, in_channels:4,  out_channels:16}
      - {k:5, in_channels:2,  out_channels:16}
      - {k:7, in_channels:1,  out_channels:16}
    gemm_tile: {M:8, N:12, K:32}
    mac_energy_pJ: 1.2
    reg_file_bytes: 256
  buffers:
    global:
      count: 6
      size_kb: 256
      energy_per_access_pJ: 8
    ps_fifo_energy_pJ: 0.5
  interconnect:
    vertical_hop_ps_energy_pJ: 0.2
    bandwidth_bytes_per_cycle: 16
  freq_MHz: 800
  data_type:
    int8: {bits:8, mac_energy_scale:1.0}
    fp16: {bits:16, mac_energy_scale:2.2}

[Latency 估算核心公式]
Let:
  T_mac = (#MAC_total) / (usable_MAC_per_cycle)
  usable_MAC_per_cycle ~ (#active_PEs) * 12   // 12 為每 micro-kernel 乘加數
  PS_chain_overhead = (chain_length - 1) * ps_latency
  Wave_overhead = (#reduction_waves - 1) * writeback_readback_latency
總 Latency ≈ Σ(各 tile block T_mac + PS_chain_overhead + Wave_overhead + load/store hide_penalty)

[Energy 估算]
E_total =
  MAC_energy = (#MAC_total) * E_mac
+ IFM_read_energy = (#IFM_reads) * E_buf
+ Weight_read_energy = (#weights_total * replication_factor) * E_rf
+ PS_pass_energy = (#partial_sum_hops) * E_ps_hop
+ Global_store/load = (#feature_map_writes + #reads) * E_buf
(可細分 report)

[演算法流程摘要]
1. Parse workload
2. Derive output dims
3. Linearize reduction (R = Cin*Kh*Kw or K)
4. Compute num_reduction_slices = ceil(R/12)
5. Slice scheduling:
     group into waves = ceil(num_reduction_slices / 7)
6. Decide horizontal mapping policy:
     if Cout_tiles >= 4 → use channel-first
     else spatial-first
7. Distribute outer loops across Arrays (round-robin or load-balance)
8. Generate IR
9. Resource / reuse 分析：計算 IFM reuse factor per Array, Weight replication factor
10. Latency model
11. Energy model
12. Emit artifacts

[Heuristic vs Optimal]
- 初版採 greedy heuristic。
- 可選擇:
  Option ILP: 目標 minimize (Latency_weight * Latency + Energy_weight * Energy)
  Decision variables: tile sizes, mapping choices (channel-first 或 spatial-first), wave partition。
- 加入約束：buffer 容量、PS chain ≤ 7。

[擴充]
- 支援 depthwise / group conv：先將 group 分割為獨立子工作負載
- 支援 dilation：展開至 linearized filter 時插入 zero-masked weights
- 支援混合精度：在 hw_config.yaml 中對不同 data_type 調整 energy scaling。

[錯誤處理 / 限制]
- 若 (Cin*Kh*Kw) > 12*7*MaxWavesLimit 且 buffer 不足暫存 PS，回報不可行
- 若 需要的同時活躍 output channel > 16 → 需要 channel tiling，否則拒絕
- Self-check: sanity: reconstructed MAC_total = N * Cout * H_out * W_out * Cin * Kh * Kw。

[範例 (簡化)]
Input Conv2D: N=1, Cin=64, Cout=128, Kh=3, Kw=3
R = 64*3*3 = 576
num_reduction_slices = 576/12 = 48
waves = 48 / 7 = 7 (最後一 wave 48-6*7=6 slices)
Cout_tiles = 128/16 = 8
Mapping:
  - Arrays 分 6 個處理 6 個 Cout_tiles (前六)
  - 剩餘 2 個 Cout_tiles → 第二輪 (temporal on cluster)
  - 每 Array: 水平 4 cols 映射 4 Cout tiles (time-multiplex if <4 剩餘)
  - 垂直 7 rows 累加 7 slices → 每 wave 完成後 寫回 partial / 繼續
估算 schedule & energy 後輸出報告。

==============================
快速開始 (Quick Start)
==============================
安裝 (在專案根目錄)：
  uv run ha-analytical --workload python/analytical/workload_examples/conv_case1.json --emit-hw-config

輸出 (預設至 python/analytical/outputs/):
  tiling_parameters.json   # tiling 參數
  schedule_ir.json         # schedule IR
  analysis_report.json     # latency / energy 粗估
  hw_config.yaml           # (若加 --emit-hw-config)

==============================
最終產物檔案列表 (Implemented Layout)
==============================
python/analytical/
  __init__.py
  cli.py                     # ha-analytical 入口
  gen_tiling.py              # 產生 KcFold=12, OFold=16 為核心的 tiling
  mapper.py                  # 決定空間/時間映射 (channel-first / spatial-first)
  ir_builder.py              # 生成簡化 schedule IR 結構
  cost_model.py              # 延遲/能耗粗估 (MAC / chain / wave)
  exporters/
    emit_ir.py              # 輸出 IR JSON
    emit_hw_config.py       # 輸出硬體設定 YAML (樣板)
    emit_report.py          # 一次輸出全部分析產物
  workload_examples/
    conv_case1.json         # 範例工作負載
  outputs/ (執行後產生)
    tiling_parameters.json
    schedule_ir.json
    analysis_report.json
    hw_config.yaml (選用)
  readme.txt

==============================
CLI 使用範例
==============================
1) 只做 heuristic tiling + 分析
   uv run ha-analytical --workload python/analytical/workload_examples/conv_case1.json

2) 同時輸出硬體設定樣板
   uv run ha-analytical --workload python/analytical/workload_examples/conv_case1.json --emit-hw-config

==============================
檔案間流程 (Pipeline)
==============================
workload(JSON/YAML) -> gen_tiling -> mapper -> ir_builder -> cost_model -> exporters

==============================
核心 Heuristic 摘要
==============================
- Reduction 線性化: R=Cin*Kh*Kw (Conv) / K (GEMM) → 切 12
- 垂直 7 rows: reduction slice chain (waves=ceil(num_slice/7))
- 水平 4 cols: 若 Cout_tiles>=4 用 channel-first 否則 spatial-first
- 6 arrays: 依序填滿 Cout tile group; 超出部分 temporal 輪次

==============================
限制 / 待辦 (TODO)
==============================
- Buffer 容量約束尚未檢查
- IFM/Weight 詳細能耗模型簡化
- 尚未提供 ILP 最佳化 (--opt heuristic 唯一)
- 未實作 JSON Schema 驗證

// ============================================================================
