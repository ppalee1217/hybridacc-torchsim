# Hybrid Accelerator NoC 架構分析（Conv2D / GEMM / Attention 對映）

## 1. 工作負載通訊特徵摘要
- Conv2D:
  - 視窗滑動導致 Input Feature Map (IFM) 區域重用；Kernel Weight 多播 (同一 filter 送往多 PE)；Partial Sum (Psums) 需在同一輸出通道聚合。
  - 常見資料流：Weight-stationary (WS), Output-stationary (OS), Row/Line-stationary (RS)。
  - Note: 目前設計 PE 內部是 WS, 但網路多播可支援 OS/RS。
- GEMM:
  - C = A (MxK) * B (KxN)，可分割成 (Tm×Tk)(Tk×Tn) tiles；A 行向、B 列向多播；部分和在 C tile 累加。
- Attention (以單頭為例，Multi-head 平行複製):
  - S = Q K^T (近似 all-to-all block-wise)；softmax 行內歸一 (需要 reduction / max / sum)；O = softmax(S) V (再度類似 GEMM)。
  - 若採 Blocked Attention：Q, K, V 分成 Bq, Bk, Bv；QK^T 變成多個 (Bq × Bk)；需要 (1) 逐塊 Score 計算、(2) 行向 max/sum reduce、(3) 再乘 V 塊並累加。
- 原語需求：
  1. Multicast（權重 / A 行 / B 列 / V 區塊）
  2. Reduction（Psum、softmax max/sum）
  3. All-to-all（Q-K 區塊交互，可拆成多播＋局部計算）
  4. Pipeline streaming（捲積滑窗、Systolic 風格 GEMM）
  5. QoS / 流控：避免 reduction 與大量多播互相阻塞 → 需 VC 分類 (e.g., data / control / reduction)

---

## 2. 架構 A：分層增強 2D Mesh (Hierarchical Multicast Mesh, H-Mesh)

### 2.1 拓樸與關鍵特性
- 基礎層：正規 2D Mesh (X×Y PEs)。
- 升級：
  1. 每 k×k 子區域形成 Cluster，Cluster Router 內建多播樹 (本地複製)。
  2. 跨 Cluster 多播採 Two-Level：源頭 → 各 Cluster 根節點 → 本地扇出。
  3. 硬體支援 In-Network Reduction：路由器中繞送相同目的座標且標記 reduce 的 flits 做加法合併（對 Psum/softmax，不需要回到 DRAM）。
  4. Virtual Channels (VCs)：(a) 普通資料 (b) 多播控制 (c) Reduction 流；避免 HoL blocking。
- Router 增強：多播複製 buffer、簡單樹狀記憶路由表 (bitmask of output ports)。

### 2.2 映射策略
- Conv2D:
  - Weight-stationary：每 Cluster 放多個輸出通道；同一 filter weight 由 cluster root 多播到 cluster 內 PE；IFM patch 由水平/垂直滑動透過相鄰 Mesh hop 傳遞。
  - Output-stationary：同一輸出像素分配到單 PE，weights 多播行/列；Psum 本地完成減少 reduction 需量。
  - Row-stationary：行片段 (stripe) 沿 X 軸排布；需要行內 shift → Mesh X 軸 pipeline。
- GEMM:
  - A 行塊沿 Y 廣播；B 列塊沿 X 廣播；C tile 分散在網格；對同一 K 分塊 (Tk) 的部分和於 PE 本地累加直到 K 填滿 → 減少網路 Psum。
- Attention:
  - QK^T：將 Q 塊按行區域化 (沿 Y)，K 塊按列 (沿 X)；形成與 GEMM 類似的 outer-product 傳播。
  - softmax:
    1. 行內 max：以在行所覆蓋的 PEs 啟動 reduction 樹（沿 X）。
    2. 行內 sum：同樣路徑 (reuse reduction network)。
  - 乘 V：softmax(QK^T) 視為中間 M×N 矩陣乘 V (N×D)，再回到 GEMM pattern。
- Blocked Attention：藉由限制 Cluster 大小減少全域壅塞。

### 2.3 分析模型 (近似)
- Hop 延遲：Lhop = (router_latency + serialization)。
- 單播訊息延遲：T_unicast ≈ (Hx + Hy)*Lhop + queue_delay。
- 多播成本：T_multicast ≈ T_root_setup + (depth_tree)*Lhop (Two-Level 近似 log_k + inter-cluster hops)。
- 有效多播節省：Traffic_reduction ≈ (#receivers - 1) * payload - (複製開銷)。
- In-network reduction latency：T_reduce ≈ (log_fanout)*Lhop + op_latency (在途中累加)。
- Link 利用率：U = offered_load / (link_bw * freq)；需 U < 0.6~0.7 避免爆延遲 (M/M/1 擬合)。

### 2.4 優缺
- 優：規律，實體佈局簡單；多播 / reduction 融合；擴展性好。
- 弱：Worst-case all-to-all (Attention 大 K,Q) 仍可能產生熱點；需要額外多播/Reduction 硬體。

<!-- 新增: 架構 A 視覺化 -->
<div>
<p><b>圖 A-1 層次化多播 Mesh</b> (Cluster 內本地多播, Cluster Root 兩層傳播)</p>
<svg width="760" height="300" style="background:#fff;font-family:sans-serif" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <pattern id="grid" width="40" height="40" patternUnits="userSpaceOnUse">
      <rect width="40" height="40" fill="none" stroke="#ddd"/>
    </pattern>
    <marker id="arrow" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 Z" fill="#333"/>
    </marker>
  </defs>
  <rect x="10" y="10" width="520" height="260" fill="url(#grid)" stroke="#aaa"/>
  <!-- Clusters -->
  <!-- Use thicker border for cluster -->
  <g stroke="#2b6" fill="none" stroke-width="2">
    <rect x="30" y="30" width="120" height="120" rx="6"/>
    <rect x="170" y="30" width="120" height="120" rx="6"/>
    <rect x="30" y="170" width="120" height="80" rx="6"/>
    <rect x="170" y="170" width="120" height="80" rx="6"/>
    <rect x="310" y="30" width="120" height="120" rx="6"/>
    <rect x="310" y="170" width="120" height="80" rx="6"/>
    <rect x="450" y="30" width="60" height="120" rx="6"/>
    <rect x="450" y="170" width="60" height="80" rx="6"/>
  </g>
  <!-- PEs (small squares) -->
  <g fill="#58c" stroke="#135" stroke-width="0.5">
    <!-- function to create grid manually -->
    <!-- cluster 1 -->
    <!-- We'll just scatter some -->
    <rect x="45" y="45" width="18" height="18"/><rect x="70" y="45" width="18" height="18"/><rect x="95" y="45" width="18" height="18"/><rect x="120" y="45" width="18" height="18"/>
    <rect x="45" y="70" width="18" height="18"/><rect x="70" y="70" width="18" height="18"/><rect x="95" y="70" width="18" height="18"/><rect x="120" y="70" width="18" height="18"/>
    <rect x="45" y="95" width="18" height="18"/><rect x="70" y="95" width="18" height="18"/><rect x="95" y="95" width="18" height="18"/><rect x="120" y="95" width="18" height="18"/>
    <!-- cluster roots (green) -->
  </g>
  <!-- Cluster Roots -->
  <g>
    <circle cx="90" cy="125" r="10" fill="#2b6" stroke="#083"/>
    <circle cx="230" cy="125" r="10" fill="#2b6" stroke="#083"/>
    <circle cx="350" cy="125" r="10" fill="#2b6" stroke="#083"/>
    <circle cx="480" cy="125" r="10" fill="#2b6" stroke="#083"/>
  </g>
  <!-- Inter-cluster links -->
  <g stroke="#555" stroke-width="2">
    <line x1="100" y1="125" x2="220" y2="125" marker-end="url(#arrow)"/>
    <line x1="240" y1="125" x2="340" y2="125" marker-end="url(#arrow)"/>
    <line x1="360" y1="125" x2="470" y2="125" marker-end="url(#arrow)"/>
  </g>
  <!-- Multicast injection -->
  <g stroke="#d22" stroke-width="2">
    <circle cx="30" cy="125" r="8" fill="#d22"/>
    <line x1="38" y1="125" x2="80" y2="125" marker-end="url(#arrow)"/>
  </g>
  <!-- Intra-cluster fanout example -->
  <g stroke="#d22" stroke-dasharray="4 3">
    <line x1="90" y1="115" x2="90" y2="105"/>
    <line x1="90" y1="115" x2="70" y2="115"/>
    <line x1="90" y1="135" x2="75" y2="150"/>
    <line x1="90" y1="135" x2="110" y2="150"/>
  </g>
  <!-- Reduction path -->
  <g stroke="#f80" stroke-width="2">
    <polyline points="470,125 360,125 240,125 100,125" fill="none" marker-end="url(#arrow)"/>
  </g>
  <rect x="550" y="20" width="190" height="250" fill="#fafafa" stroke="#ccc"/>
  <text x="560" y="40" font-size="14" font-weight="bold">圖例 Legend</text>
  <rect x="565" y="55" width="14" height="14" fill="#58c" stroke="#135"/><text x="585" y="67" font-size="12">PE</text>
  <circle cx="572" cy="90" r="8" fill="#2b6" stroke="#083"/><text x="585" y="94" font-size="12">Cluster Root</text>
  <circle cx="572" cy="115" r="8" fill="#d22"/><text x="585" y="119" font-size="12">多播源頭</text>
  <line x1="562" y1="140" x2="580" y2="140" stroke="#d22" stroke-width="2" marker-end="url(#arrow)"/><text x="585" y="143" font-size="12">多播方向</text>
  <line x1="562" y1="165" x2="580" y2="165" stroke="#f80" stroke-width="2" marker-end="url(#arrow)"/><text x="585" y="168" font-size="12">Reduction 回傳</text>
  <line x1="562" y1="190" x2="580" y2="190" stroke="#555" stroke-width="2" marker-end="url(#arrow)"/><text x="585" y="193" font-size="12">Inter-Cluster Link</text>
</svg>

<p><b>圖 A-2 In-Network Reduction (加總 Psums / softmax)</b></p>
<svg width="760" height="180" style="background:#fff;font-family:sans-serif" xmlns="http://www.w3.org/2000/svg">
  <defs><marker id="arrow2" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
    <path d="M0,0 L8,4 L0,8 Z" fill="#333"/></marker></defs>
  <!-- chain of routers -->
  <g stroke="#999" fill="#eef">
    <rect x="40" y="40" width="60" height="60" rx="6"/><rect x="140" y="40" width="60" height="60" rx="6"/>
    <rect x="240" y="40" width="60" height="60" rx="6"/><rect x="340" y="40" width="60" height="60" rx="6"/>
    <rect x="440" y="40" width="60" height="60" rx="6"/><rect x="540" y="40" width="60" height="60" rx="6"/>
  </g>
  <!-- Psum partial contributions -->
  <g fill="#58c">
    <circle cx="70" cy="30" r="8"/><circle cx="170" cy="30" r="8"/><circle cx="270" cy="30" r="8"/>
    <circle cx="370" cy="30" r="8"/><circle cx="470" cy="30" r="8"/>
  </g>
  <!-- arrows -->
  <g stroke="#f80" stroke-width="2">
    <line x1="100" y1="70" x2="140" y2="70" marker-end="url(#arrow2)"/>
    <line x1="200" y1="70" x2="240" y2="70" marker-end="url(#arrow2)"/>
    <line x1="300" y1="70" x2="340" y2="70" marker-end="url(#arrow2)"/>
    <line x1="400" y1="70" x2="440" y2="70" marker-end="url(#arrow2)"/>
    <line x1="500" y1="70" x2="540" y2="70" marker-end="url(#arrow2)"/>
  </g>
  <!-- reduction accumulation values -->
  <g fill="#f80" font-size="12" font-weight="bold">
    <text x="48" y="85">p0</text>
    <text x="148" y="85">p0+p1</text>
    <text x="248" y="85">..+p2</text>
    <text x="348" y="85">..+p3</text>
    <text x="448" y="85">..+p4</text>
    <text x="548" y="85">Result</text>
  </g>
  <text x="40" y="125" font-size="12">路由器逐跳累加 → 減少回寫與總流量</text>
</svg>
</div>

---

## 3. 架構 B：Cluster Crossbar + Global Ring (Hybrid Hierarchical)

### 3.1 拓樸
- 將 PEs 分成 C 個 Cluster；Cluster 內部：本地小型 Crossbar / Fully-connected (低延遲多播)。
- Cluster 間：雙向分段 Global Ring (可插入 2~4 個 Gateway Router 支援部分繞行/虛擬通道)；可視規模改成多環 (X,Y 分離)。
- 可選：在 Gateway 上實作 multicast replication + reduction buffer。

### 3.2 映射策略
- Conv2D:
  - 在 Crossbar 內快速多播 kernel；IFM patch 由 Ring 以 pipeline 傳遞到下一 Cluster（stream sliding）。
- GEMM:
  - A 分塊在 ring 方向流動；B 分塊反向流；各 Cluster 透過本地 crossbar 將到達的 A/B 送到多 PE → 類似雙向流 systolic。
- Attention:
  - Q, K 區塊分別沿 Ring 相向流動 (減少全域同時發送風暴)。
  - softmax reduction：Cluster 內先本地 reduce → Gateway 逐點在 ring 上進行分層 reduce (分段 + 轉發) → 降低全域流量。
  - 乘 V：複用與 GEMM 相似雙向流機制。
- Blocked Attention：使單次活躍的 Bq×Bk 集減少，透過排程在 Ring 上時槽化 (time-sliced windows)。

### 3.3 定量模型
- Ring 總延遲：T_ring ≈ (hops_ring * (router_latency + serialization)) + contention。
- 單迴圈吞吐：BW_eff ≈ (link_bw * freq)/(1 + utilization_penalty)。
- Cluster 多播延遲：T_c_mc ≈ replication_latency + 1~2 cycles (crossbar)。
- 分層 reduce：T_reduce ≈ T_cluster_local + T_ring_accumulate (可 pipeline)。
- 若有雙向 (counter-rotating) 環：平均 hops ≈ N_cluster/4。

### 3.4 優缺
- 優：實作簡單，版圖可沿晶片邊緣佈局；Cluster 內高頻寬；雙向流適合 GEMM/Attention pipeline。
- 弱：當多個 Cluster 同時大量多播 (weights + K/V) 時，Ring 可能成為瓶頸；擴展超過 ~64–128 PEs 需多環或升級為 Hierarchical Ring-of-Rings / 小 Fat-tree。

<!-- 新增: 架構 B 視覺化 -->
<div>
<p><b>圖 B-1 Cluster + 雙向 Ring 拓樸</b></p>
<svg width="780" height="300" style="background:#fff;font-family:sans-serif" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <marker id="arrB" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 Z" fill="#333"/>
    </marker>
  </defs>
  <!-- Ring path -->
  <circle cx="340" cy="150" r="130" fill="none" stroke="#555" stroke-width="3" stroke-dasharray="8 5"/>
  <circle cx="340" cy="150" r="115" fill="none" stroke="#555" stroke-width="2" stroke-dasharray="5 6"/>
  <text x="470" y="145" font-size="12" fill="#555">Outer Ring (CW)</text>
  <text x="470" y="165" font-size="12" fill="#555">Inner Ring (CCW)</text>
  <!-- Clusters (rectangles on ring) -->
  <g stroke="#2b6" fill="#f7fff7">
    <rect x="320" y="20" width="40" height="40" rx="6"/>
    <rect x="460" y="110" width="40" height="40" rx="6"/>
    <rect x="320" y="240" width="40" height="40" rx="6"/>
    <rect x="180" y="110" width="40" height="40" rx="6"/>
    <rect x="430" y="50" width="40" height="40" rx="6"/>
    <rect x="210" y="50" width="40" height="40" rx="6"/>
    <rect x="210" y="180" width="40" height="40" rx="6"/>
    <rect x="430" y="180" width="40" height="40" rx="6"/>
  </g>
  <!-- Crossbar internal (mini) -->
  <g fill="#58c" stroke="#135" stroke-width="0.5">
    <rect x="327" y="27" width="10" height="10"/><rect x="343" y="27" width="10" height="10"/><rect x="335" y="41" width="10" height="10"/>
    <rect x="467" y="117" width="10" height="10"/><rect x="483" y="117" width="10" height="10"/><rect x="475" y="131" width="10" height="10"/>
    <rect x="327" y="247" width="10" height="10"/><rect x="343" y="247" width="10" height="10"/><rect x="335" y="261" width="10" height="10"/>
  </g>
  <!-- Data flow arrows (A, B streams) -->
  <g stroke="#d22" stroke-width="2">
    <path d="M180 150 Q 260 40 340 20" fill="none" marker-end="url(#arrB)"/>
    <path d="M500 150 Q 420 260 360 240" fill="none" marker-end="url(#arrB)"/>
  </g>
  <g stroke="#1a7" stroke-width="2">
    <path d="M500 150 Q 420 40 360 20" fill="none" marker-end="url(#arrB)"/>
    <path d="M180 150 Q 260 260 340 240" fill="none" marker-end="url(#arrB)"/>
  </g>
  <rect x="600" y="20" width="170" height="260" fill="#fafafa" stroke="#ccc"/>
  <text x="610" y="40" font-size="14" font-weight="bold">圖例</text>
  <rect x="612" y="55" width="16" height="16" fill="#58c" stroke="#135"/><text x="635" y="67" font-size="12">PE</text>
  <rect x="612" y="80" width="16" height="16" fill="#f7fff7" stroke="#2b6"/><text x="635" y="92" font-size="12">Cluster 邊界</text>
  <line x1="610" y1="115" x2="640" y2="115" stroke="#d22" stroke-width="2" marker-end="url(#arrB)"/><text x="645" y="118" font-size="12">A / Weights 流</text>
  <line x1="610" y1="140" x2="640" y2="140" stroke="#1a7" stroke-width="2" marker-end="url(#arrB)"/><text x="645" y="143" font-size="12">B / K / V 流</text>
  <line x1="610" y1="165" x2="640" y2="165" stroke="#555" stroke-width="3" stroke-dasharray="8 5"/><text x="645" y="169" font-size="12">外圈 Ring (CW)</text>
  <line x1="610" y1="190" x2="640" y2="190" stroke="#555" stroke-width="2" stroke-dasharray="5 6"/><text x="645" y="194" font-size="12">內圈 Ring (CCW)</text>
</svg>

<p><b>圖 B-2 分層 Reduction 與 Attention Block 排程</b></p>
<svg width="780" height="210" style="background:#fff;font-family:sans-serif" xmlns="http://www.w3.org/2000/svg">
  <!-- Clusters -->
  <g stroke="#2b6" fill="#f7fff7">
    <rect x="40" y="30" width="120" height="70" rx="6"/>
    <rect x="200" y="30" width="120" height="70" rx="6"/>
    <rect x="360" y="30" width="120" height="70" rx="6"/>
    <rect x="520" y="30" width="120" height="70" rx="6"/>
  </g>
  <!-- local reduce -->
  <g fill="#58c" stroke="#135" stroke-width="0.5">
    <rect x="55" y="45" width="18" height="18"/><rect x="80" y="45" width="18" height="18"/><rect x="105" y="45" width="18" height="18"/>
    <rect x="215" y="45" width="18" height="18"/><rect x="240" y="45" width="18" height="18"/><rect x="265" y="45" width="18" height="18"/>
    <rect x="375" y="45" width="18" height="18"/><rect x="400" y="45" width="18" height="18"/><rect x="425" y="45" width="18" height="18"/>
    <rect x="535" y="45" width="18" height="18"/><rect x="560" y="45" width="18" height="18"/><rect x="585" y="45" width="18" height="18"/>
  </g>
  <!-- local reduce arrows -->
  <g stroke="#f80" stroke-width="2">
    <line x1="63" y1="63" x2="96" y2="63"/>
    <line x1="223" y1="63" x2="256" y2="63"/>
    <line x1="383" y1="63" x2="416" y2="63"/>
    <line x1="543" y1="63" x2="576" y2="63"/>
  </g>
  <!-- cluster outputs -->
  <g fill="#f80">
    <circle cx="115" cy="65" r="8"/>
    <circle cx="275" cy="65" r="8"/>
    <circle cx="435" cy="65" r="8"/>
    <circle cx="595" cy="65" r="8"/>
  </g>
  <!-- ring accumulate -->
  <g stroke="#f80" stroke-width="2" fill="none">
    <path d="M123 65 C 180 65, 180 65, 267 65" marker-end="url(#arrB)"/>
    <path d="M283 65 C 340 65, 340 65, 427 65" marker-end="url(#arrB)"/>
    <path d="M443 65 C 500 65, 500 65, 587 65" marker-end="url(#arrB)"/>
  </g>
  <defs>
    <marker id="arrB" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 Z" fill="#f80"/>
    </marker>
  </defs>
  <text x="40" y="130" font-size="12">Phase 1: 各 Cluster 本地 reduce → 產生局部結果</text>
  <text x="40" y="150" font-size="12">Phase 2: Ring 分段累加 (流水) → 全域結果</text>
  <text x="40" y="170" font-size="12">可與下一批 Score 計算 overlap，減少等待。</text>
</svg>
</div>

---

## 4. 架構 C：可重組 Systolic / Circuit-Switched Overlay (Reconfig Systolic NoC)

### 4.1 觀念
- 將 NoC 在時間 (TDM) 與空間上建立「資料流通道」：設定 Route Table 形成單/雙向 Systolic 路徑或多播樹；在配置期 (reconfiguration phase) 下發控制封包，建立暫存 switch state（近似簡化電路交換）。
- 路由器簡化：只需 small config RAM + pipeline registers；資料階段無需標頭 (headerless streaming) → 低 per-hop 延遲。
- 支援模式：
  1. GEMM Mode：經典 2D systolic (A 往右推, B 往下推, Psum 累加)。
  2. Conv2D Mode：IFM 列 shift + kernel 行/列廣播；多播通路預先配置為樹。
  3. Attention Mode：
     - Phase 1 (QK^T)：配置多組平行 outer-product 通道；每組 systolic block 專注於對 (Q_block, K_block)。
     - Phase 2 (Reduction)：切換成樹型 reduce 網 (TDM slot 分配)；輸出 max/sum。
     - Phase 3 (softmax*V)：再切換成 GEMM/outer-product systolic 通路。
- 若需要 block-sparse：只建立對應非零 block 的通路節省能耗。

### 4.2 映射/排程
- 以「Phase 表」描述 (mode_id, active_links, duration_cycles)；調度器確保前一階段 buffer drain 後切換。
- Conv2D：
  - Kernel 固定於特定 PE 行；IFM 滑動視窗透過行向 pipeline；輸出通道分攤在多列。
- GEMM：
  - Tile 尺寸 (Tm,Tn,Tk) 決定 systolic array 尺寸；超出部分以多輪 (wavefront) 疊加。
- Attention：
  - Q,K 被分成 (Bq,Bk)；建立多個 systolic pairs；phase overlap：當 pair i 計算 QK^T 時，pair i-1 進行 reduction，pair i-2 進行 softmax*V → 形成三階段 pipeline (Latency hiding)。
- Reconfiguration Overhead：T_reconf ≈ (#routers * cfg_bits)/(cfg_bus_bw * freq)。需約 < 1~2% 工作負載時間；可透過 (a) 重複使用模式表 (b) 局部更新 (c) 壓縮配置。

### 4.3 分析
- 資料模式延遲：T_stream ≈ pipeline_depth + data_elements / per_cycle_throughput。
- Per-hop 幾乎 = 1 cycle (無路由決策)，因此對細粒度 tile 高效率。
- 多播：以樹狀鏈接預配置 → latency ≈ tree_depth。
- Reduction：同樣樹通路，pipeline reduce → throughput = 1 element/cycle (若樹平衡)。

### 4.4 優缺
- 優：近似專用資料流網路，能耗與延遲最低，適合規律矩陣核 (GEMM / Conv) 與分相 Attention。
- 弱：動態/非規律 (稀疏不穩定) 需頻繁 reconfig；控制複雜；debug 難度高。

<!-- 新增: 架構 C 視覺化 -->
<div>
<p><b>圖 C-1 Reconfig 通路 三階段 Attention Pipeline</b></p>
<svg width="780" height="320" style="background:#fff;font-family:sans-serif" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <marker id="arrC" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 Z" fill="#333"/>
    </marker>
  </defs>
  <text x="40" y="30" font-size="14" font-weight="bold">Phase 1: QK^T Blocks</text>
  <text x="290" y="30" font-size="14" font-weight="bold">Phase 2: Reduce (max/sum)</text>
  <text x="560" y="30" font-size="14" font-weight="bold">Phase 3: (softmax) * V</text>
  <g stroke="#2b6" fill="#f0fff0">
    <rect x="40" y="50" width="180" height="90" rx="8"/>
    <rect x="40" y="155" width="180" height="90" rx="8" fill="#f0f8ff"/>
  </g>
  <g stroke="#f80" fill="#fff7e6">
    <rect x="290" y="80" width="180" height="90" rx="8"/>
  </g>
  <g stroke="#2b6" fill="#f0fff0">
    <rect x="560" y="80" width="180" height="170" rx="8"/>
  </g>
  <g fill="#58c" stroke="#135" stroke-width="0.5">
    <rect x="55" y="65" width="18" height="18"/><rect x="80" y="65" width="18" height="18"/><rect x="105" y="65" width="18" height="18"/>
    <rect x="55" y="90" width="18" height="18"/><rect x="80" y="90" width="18" height="18"/><rect x="105" y="90" width="18" height="18"/>
    <rect x="55" y="115" width="18" height="18"/><rect x="80" y="115" width="18" height="18"/><rect x="105" y="115" width="18" height="18"/>
    <rect x="55" y="170" width="18" height="18"/><rect x="80" y="170" width="18" height="18"/><rect x="105" y="170" width="18" height="18"/>
    <rect x="55" y="195" width="18" height="18"/><rect x="80" y="195" width="18" height="18"/><rect x="105" y="195" width="18" height="18"/>
  </g>
  <g stroke="#f80" stroke-width="2">
    <line x1="305" y1="105" x2="335" y2="105" marker-end="url(#arrC)"/>
    <line x1="305" y1="145" x2="335" y2="145" marker-end="url(#arrC)"/>
    <line x1="365" y1="125" x2="405" y2="125" marker-end="url(#arrC)"/>
    <polyline points="335,105 355,115 335,145" fill="none"/>
  </g>
  <g fill="#58c" stroke="#135" stroke-width="0.5">
    <rect x="575" y="95" width="18" height="18"/><rect x="600" y="95" width="18" height="18"/><rect x="625" y="95" width="18" height="18"/>
    <rect x="575" y="120" width="18" height="18"/><rect x="600" y="120" width="18" height="18"/><rect x="625" y="120" width="18" height="18"/>
    <rect x="575" y="145" width="18" height="18"/><rect x="600" y="145" width="18" height="18"/><rect x="625" y="145" width="18" height="18"/>
    <rect x="575" y="170" width="18" height="18"/><rect x="600" y="170" width="18" height="18"/><rect x="625" y="170" width="18" height="18"/>
  </g>
  <g stroke="#d22" stroke-width="2">
    <path d="M125 85 C 180 90, 260 110, 290 115" fill="none" marker-end="url(#arrC)"/>
    <path d="M125 190 C 180 195, 260 135, 290 130" fill="none" marker-end="url(#arrC)"/>
  </g>
  <g stroke="#1a7" stroke-width="2">
    <path d="M470 125 C 500 125, 545 125, 560 125" fill="none" marker-end="url(#arrC)"/>
  </g>
  <g stroke="#f80" stroke-width="2">
    <path d="M470 125 C 515 155, 545 170, 560 180" fill="none" marker-end="url(#arrC)"/>
  </g>
  <text x="40" y="260" font-size="12">Phase Overlap：QK 計算 / Reduction / softmax*V 串接。</text>
  <text x="40" y="280" font-size="12">控制僅於相位邊界重構，資料期 header-less 高效率。</text>
</svg>

<p><b>圖 C-2 GEMM 2D Systolic 流 (A 往右, B 往下, Psums 對角傳遞)</b></p>
<svg width="780" height="220" style="background:#fff;font-family:sans-serif" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <marker id="arrCs" markerWidth="8" markerHeight="8" refX="6" refY="4" orient="auto">
      <path d="M0,0 L8,4 L0,8 Z" fill="#333"/>
    </marker>
  </defs>
  <g fill="#eef" stroke="#135">
    <rect x="80" y="40" width="40" height="40"/><rect x="130" y="40" width="40" height="40"/><rect x="180" y="40" width="40" height="40"/><rect x="230" y="40" width="40" height="40"/>
    <rect x="80" y="90" width="40" height="40"/><rect x="130" y="90" width="40" height="40"/><rect x="180" y="90" width="40" height="40"/><rect x="230" y="90" width="40" height="40"/>
    <rect x="80" y="140" width="40" height="40"/><rect x="130" y="140" width="40" height="40"/><rect x="180" y="140" width="40" height="40"/><rect x="230" y="140" width="40" height="40"/>
    <rect x="80" y="190" width="40" height="40"/><rect x="130" y="190" width="40" height="40"/><rect x="180" y="190" width="40" height="40"/><rect x="230" y="190" width="40" height="40"/>
  </g>
  <g stroke="#d22" stroke-width="2">
    <line x1="120" y1="60" x2="130" y2="60" marker-end="url(#arrCs)"/>
    <line x1="170" y1="60" x2="180" y2="60" marker-end="url(#arrCs)"/>
    <line x1="220" y1="60" x2="230" y2="60" marker-end="url(#arrCs)"/>
    <line x1="120" y1="110" x2="130" y2="110" marker-end="url(#arrCs)"/>
    <line x1="170" y1="110" x2="180" y2="110" marker-end="url(#arrCs)"/>
    <line x1="220" y1="110" x2="230" y2="110" marker-end="url(#arrCs)"/>
  </g>
  <g stroke="#1a7" stroke-width="2">
    <line x1="100" y1="80" x2="100" y2="90" marker-end="url(#arrCs)"/>
    <line x1="150" y1="80" x2="150" y2="90" marker-end="url(#arrCs)"/>
    <line x1="200" y1="80" x2="200" y2="90" marker-end="url(#arrCs)"/>
    <line x1="250" y1="80" x2="250" y2="90" marker-end="url(#arrCs)"/>
    <line x1="100" y1="130" x2="100" y2="140" marker-end="url(#arrCs)"/>
    <line x1="150" y1="130" x2="150" y2="140" marker-end="url(#arrCs)"/>
  </g>
  <g stroke="#f80" stroke-width="2">
    <polyline points="120,80 150,110 180,140 210,170" fill="none" marker-end="url(#arrCs)"/>
    <polyline points="170,80 200,110 230,140 260,170" fill="none" marker-end="url(#arrCs)"/>
  </g>
  <rect x="420" y="40" width="200" height="140" fill="#fafafa" stroke="#ccc"/>
  <text x="430" y="60" font-size="14" font-weight="bold">圖例</text>
  <line x1="430" y1="80" x2="460" y2="80" stroke="#d22" stroke-width="2" marker-end="url(#arrCs)"/><text x="465" y="84" font-size="12">A shift</text>
  <line x1="430" y1="105" x2="460" y2="105" stroke="#1a7" stroke-width="2" marker-end="url(#arrCs)"/><text x="465" y="109" font-size="12">B shift</text>
  <line x1="430" y1="130" x2="460" y2="130" stroke="#f80" stroke-width="2" marker-end="url(#arrCs)"/><text x="465" y="134" font-size="12">Psum 傳遞</text>
  <rect x="430" y="150" width="16" height="16" fill="#eef" stroke="#135"/><text x="452" y="162" font-size="12">PE</text>
</svg>
</div>

## 5. 架構比較 (摘要)
| 面向 | A H-Mesh | B Cluster+Ring | C Reconfig Systolic |
|------|----------|----------------|---------------------|
| 多播效率 | 中高 (兩層) | 中 (跨 cluster 受 ring 限速) | 高 (專用樹) |
| Reduction | In-network 可融合 | 分層 (Cluster+Ring) | 預配置樹，高效 |
| All-to-all (Block Attention) | 需排程避免熱點 | Ring 方向雙流 | Phase 多路並行 |
| 重構開銷 | 低 | 低 | 中 (需 reconfig) |
| 擴展性 | 規律，可線性 | 需多環/階層 | 受配置寬度限制 |
| 實作複雜度 | 中 | 低~中 | 高 |

---

## 6. 選擇與調參建議
- 若重心 = 多種工作負載且需要通用性：選 A。
  - Cluster 大小 k：平衡本地多播扇出與路由器複雜度，常 4×4 或 8×4。
  - VC 數：>=3 分離 多播/Reduction/一般；link_width 視 Psum 叢集平均活躍度調整。
- 若晶片規模中等、便於快速整合：選 B。
  - Cluster PE 數：8~16；Ring 分段數：>=2 (counter-rotating)。
  - 安排 Attention blocks 為 time-sliced，避開與大型 Conv/GEMM 多播同時。
- 若目標峰值效能與能效 (GEMM/Conv/Attention 相對規律)：選 C。
  - Tile 尺寸讓 systolic 利用率 >90%；reconfig 週期與 tile wavefront 對齊。
  - 建 mode cache：常用三模式 (Conv2D, GEMM, Attention Phase) 預先燒錄。
- 混合策略：A + 在 Mesh 上疊加窄帶電路交換 side-channel 作為 Reduction/Multicast 快徑 (增加 ~10% 佈線) 亦可。

---

## 7. 簡化效能估算公式總表
- 單播延遲 (Mesh / Ring)：T ≈ hops * (Lr + Ls) + Q。
- 多播有效負載放大係數：Amplify = Sent_bytes / (payload_bytes * receivers)；越接近 1 越好。
- Reduction latency：T_red ≈ log_fanout * Lhop + Op_lat；Throughput ≈ 1 / cycle (pipeline)。
- Systolic 完成時間 (單 phase)：T_phase ≈ (warmup + data_len) / PE_parallelism。
- Attention 三相 pipeline 吞吐：Steady_state ≈ max(T_QK/block, T_softmax/block, T_V/block)。
- Link 利用率：U = (Σ traffic_i) / (link_bw * freq)；保守設計 U_target < 0.65。

---

## 8. 實作優先順序建議
1. 先以 A (H-Mesh) 做 baseline（因你已有 conv1D/GEMM PE，可快速拓展到 2D array）。
2. 加入多播 + reduction 擴充 (router output mask, accumulate unit)。
3. 對 Attention 做 Blocked 排程：限制同時活躍的 Bq×Bk 對數。
4. 若 profiling 顯示多播或 reduce 成為主瓶頸，再評估 C 的局部化 (以 partial reconfig 通路替換最熱 path)。
5. 建 analytical simulator：輸入 (tile 尺寸, mapping, traffic pattern) → 輸出 (hop dist 分佈, VC utilization, predicted latency)。校準後再迭代硬體參數。

---

## 9. 後續拓展
- 支援稀疏：在 A/C 架構加入壓縮 header 指示跳過零值；Router 跳過 buffer 排隊。
- QoS：為 softmax reduction 保留高優先 VC，減少等待導致整體 attention pipeline stall。
- 能耗監控：計算 多播/單播/空閒 cycles，做動態關閉 cluster link。

---

(完)

## 附錄 A：QoS / VC 更細部設計與範例

### A.1 類別分類範例
| 類別 | 來源 | 典型封包大小 (Bytes) | 延遲敏感 | 建議 VC | 備註 |
|------|------|----------------------|----------|--------|------|
| Reduction (softmax max/sum, psum merge) | PE → 樹 | 8~32 | 高 | VC2 | 小而頻繁 |
| Multicast Control / 樹頭部 | Host / Root | 8~16 | 中高 | VC1 | 樹展開前置 |
| Bulk Compute Data (Weights / A 行 / B 列 / V) | SRAM → PEs | 64~256 | 中 | VC0 | 占多數頻寬 |
| IFM Sliding Patch / K,V 流動 | 相鄰 PE | 32~128 | 中 | VC0 | pipeline 化 |
| 回寫 / Meta / Host Cmd | PE → Host | 8~32 | 低 (可批次) | VC3 (可選) | 背景傳送 |

最小配置：3 個 VC (VC0/1/2)。若 Host 回寫與 bulk 混擾不大，可省 VC3。

### A.2 VC Buffer 深度估算
目標：高優先類 (VC2) 排隊延遲 < T_budget。
估式：Depth ≥ ceil( Burst_pkts + λ_eff * (RTT_cycles) )
- RTT_cycles ≈ 2*(H_avg) + credit_return + 仲裁開銷
- λ_eff：該類封包平均每 cycle 到達機率
例：H_avg=6, credit_return=8 → RTT≈20；Reduction λ=0.1 pkt/cycle，Burst=2 → Depth ≥ 2 + 0.1*20 = 4 → 配 4~6 entries。
Bulk (VC0) 需吸收突發：Depth 16~32（依鏈路寬度）。

### A.3 多播 + Reduction 擴展路由器關鍵模組 (Pseudo SystemVerilog)
```systemverilog
// multicast / reduction pipeline (簡化)
always_ff @(posedge clk) begin
  if (in_valid) begin
    if (hdr.multicast) begin
      out_mask <= route_table[hdr.dest_mask]; // bitmask 輸出
      for (int p=0;p<PORTS;p++)
        if (out_mask[p]) fifo[p].enq(payload);
    end else if (hdr.reduce) begin
      // 匹配同 key 緩存累加
      if (acc_buf[hdr.key].valid) begin
        acc_buf[hdr.key].data <= acc_buf[hdr.key].data + payload;
        acc_buf[hdr.key].count++;
        if (acc_buf[hdr.key].count == hdr.fan_in)
          forward(acc_buf[hdr.key].data);
      end else begin
        acc_buf[hdr.key] <= '{valid:1,data:payload,count:1};
      end
    end else begin
      forward_unicast(next_port, payload);
    end
  end
end
```

### A.4 Arbitration 策略 (階層 + Aging)
1. 先比較 VC 優先層級 (VC2 > VC1 > VC0 > VC3)。
2. 同層使用 Round-Robin + Age bit：若等待 > AgeThresh，提升一次性 bonus。
好處：避免 long bulk stream 飢餓高優先小封包。

### A.5 動態節流 (Token Bucket 對 Bulk 流)
狀態：每輸出埠維護 tokens_bulk。
更新：tokens_bulk += refill_rate 每 N cycles。
發送 Bulk：若 tokens_bulk>pkt_cost 且 Reduction 平均排隊 <= Target → 允許；否則停發。
Pseudo:
```python
if red_queue_delay > target_delay:
    refill_rate = max(refill_rate - step, min_rate)
elif red_queue_delay < target_delay * 0.6:
    refill_rate = min(refill_rate + step, max_rate)
```

### A.6 熱點偵測與 VC Rebalance
- 監控指標：per-link VC0 utilization > 75% 且 VC2 延遲上升斜率 d(delay)/dt > 閾值。
- 動作：暫時降低該源 PE 注入 (inj_rate *= 0.8)，或將部分多播改分批 (分段樹：拆成 2 個時間槽)。

### A.7 示例：Attention softmax 行內 reduce 時序
假設行寬 W=16，採二叉樹 (fanout=2)：
Depth=log2(16)=4；Lhop=2 cycles；op_latency=1 → T_reduce ≈ 4*2+1=9 cycles。
若同時 4 行並行：需確保 VC2 pipeline 每 cycle 可推出一個 (使用 separate pipeline register + 不回壅塞 VC0)。
若未分離 VC：HoL 造成平均 queue_delay=~15 cycles → latency 變 24 cycles，吞吐下降 ~2.6×。

### A.8 配置實例 (小型 8×8 Mesh, k=4)
| 參數 | 值 |
|------|----|
| Cluster 尺寸 | 4×4 |
| VC 數 | 3 |
| Buffer 深度 (entries) | VC2=6, VC1=4, VC0=24 |
| Link Width | 128b |
| router_latency | 2 cycles |
| serialization (128b@128b flit) | 1 cycle |
| 平均 hop | 6 |
| Reduction 目標延遲 | < 50 cycles |

計算：預期 VC2 平均延遲 ≈ (hops * Lhop) + queue ≈ 6*3 + 4 ≈ 22 cycles (達標)。

### A.9 監控計數器 (Hardware Perf Counters)
| 計數器 | 用途 |
|--------|------|
| vcX_in_flits / vcX_out_flits | 利用率與阻塞比 |
| vcX_avg_qlen (滑動平均) | 判定節流觸發 |
| reduction_latency_hist | 驗證 QoS 達標 |
| multicast_dup_flits | 多播放大量化 |
| link_idle_cycles | 能耗 gating 依據 |

### A.10 Simulator 需求最小集合
輸入：traffic traces (time, src, dst set, size, class)
模組：VC 狀態機 + credit + arbiter + simple queue delay (M/D/1)
輸出：per-class latency CDF, link util, drop(若 any)，瓶頸排行。
迭代：調整 (buffer depth, VC數, throttle policy) → 找 Pareto (延遲 vs 面積)。

---

## 附錄 B：快速檢查清單 (Bring-Up Checklist)
- [ ] VC buffer ECC 或 parity（避免 silent data corruption 對 reduction）
- [ ] Multicast header 最多 output mask bits ≦ PORTS；跨 cluster 樹第二層是否拆分過大 payload
- [ ] Reduction key 空間：使用 (dst_coord + op_id) 哈希避免 collision
- [ ] Flow control：credit return path latency 校準 (模擬 vs RTL) 差異 <5%
- [ ] QoS：壓力測試：Bulk 注入 0.7 link utilization 時 reduction 95th 延遲 ≤ 2× baseline

---

## 附錄 C：面積 / 能耗 粗估 (相對)
假設 1 VC buffer entry (128b) SRAM 等效 16 bytes。
若 VC0 24 entries + VC1 4 + VC2 6 → Total entries/port = 34 → 34*16=544B。5 向 (N,E,S,W,Local) ≈ 2.7KB/router。
與增加第 4 個 VC (再 +8 entries*16*5 ≈ 640B) 比較：+23% buffer 面積，僅在 Host/Result traffic 明顯干擾時才必要。

---

(附錄完)