# Cluster Compiler 設計與使用說明書

本文件定義 Cluster Compiler 的完整設計、輸入輸出規格、Cost Model 參數、模板化 PE 程式生成機制，以及驗證流程。目標是讓前端使用者只描述運算需求，後端自動完成排程、映射、組態輸出與可載入檔生成。

## 1. 文件目標與範圍

### 1.1 目標
1. 統一說明 Conv/GEMM 的編譯流程。
2. 定義可擴充的輸入格式，避免手動維護大量 per-wave 設定。
3. 明確列出 Timing Cost Model 需要的參數，讓使用者可填值後立即投入排程探索。
4. 提供 Stage 0 Debug JSON 與 Stage 1 Loadable 的格式規範。
5. 定義 Golden Tensor 工具流程，建立端到端可驗證機制。

### 1.2 支援運算
1. Conv2D: `conv2d_1x1`, `conv2d_3x3`, `conv2d_5x5`, `conv2d_7x7`
2. GEMM: `gemm`

### 1.3 非目標
1. 不在本版文件定義訓練相關算子。
2. 不在本版文件定義動態 shape runtime JIT。
3. 不在本版文件定義跨多 Cluster 全域排程。

## 2. 核心設計決策

### 2.1 決策 A: 前端只描述運算，PE 程式由後端模板生成

建議採用此模式作為主方案：
1. 前端使用者輸入 operation 與 tensor shape，不直接寫 PE ASM。
2. 後端維護 kernel template registry，將 `operation` 對應到模板。
3. Compiler 在排程完成後注入模板參數，輸出最終 ASM 與機器碼。

優點：
1. 避免 `config.json` 與 `pe_program.asm` 不一致。
2. loop count 不再人工同步，改由編譯器根據 tile 自動推導。
3. 模板可版本化管理，便於驗證與回溯。

### 2.2 決策 B: 用「迴圈描述」取代「per-wave 全展開」

不建議將上千 wave 全部展平成明細。改為保存可重建的迴圈描述：
1. `loop_dims`: 各層迭代次數
2. `stride_bytes`: 各層地址步進
3. `residual_policy`: 尾塊處理策略

這可大幅縮短輸出檔長度，並保留完整可重建性。

## 3. 編譯器分層架構

### 3.1 模組分層
1. Frontend Parser
2. IR Builder
3. DSE + Cost Model
4. Mapper (Compute + Memory + NoC)
5. Backend Codegen (Template + Assembler + Packager)
6. Validation Tools (Golden Generator + Checker)

### 3.2 建議 C++ 目錄結構

```text
cluster_compiler/
    include/
        cc/api.hpp
        cc/ir/*.hpp
        cc/cost_model/*.hpp
        cc/backend/*.hpp
    src/
        frontend/
        ir/
        passes/
        cost_model/
        backend/
        cli/
    kernels/
        registry.yaml
        conv2d_1x1.template.asm
        conv2d_3x3.template.asm
        conv2d_5x5.template.asm
        conv2d_7x7.template.asm
        gemm.template.asm
    schemas/
        op.schema.json
        stage0.schema.json
        hw_constraint.schema.json
    tools/
        generate_golden.py
        compare_tensor.py
```

## 4. 編譯流程與 Pass 順序

### 4.1 總流程
1. Parse input op json
2. Validate shape and type
3. Ultra mode eligibility check
4. Generate candidate tilings
5. Evaluate candidates by timing cost model
6. Select optimal schedule
7. Compute mapping to PE array
8. SRAM bank/group partition
9. Memory space mapping
10. Generate AGU loops/strides
11. Instantiate PE template
12. Assemble and emit stage0/stage1

### 4.2 為何這個順序
1. Ultra mode 會改變 mapping 與 memory 模式，需先判定。
2. schedule 決定 tile 型態，tile 才能驅動 bank 分配與 AGU 配置。
3. 先完成 mapping 再生成程式，才能填入正確 loop count 與地址。

## 5. IR 設計 (核心資料模型)

### 5.1 OpIR
描述使用者輸入語意：
1. operation type
2. tensor shapes
3. data type
4. stride/padding/dilation
5. optional policy flags (ultra allow/disallow)

### 5.2 ScheduleIR
描述排程結果：
1. tile sizes (`tile_n`, `tile_c`, `tile_h`, `tile_w`, `tile_k`, `tile_m`)
2. loop order
3. pipeline overlap policy (dma/compute overlap)
4. selected ultra mode

### 5.3 MappingIR
描述硬體映射：
1. PE grid assignment
2. NoC route mode
3. scan chain control
4. plane enable/mode

### 5.4 MemoryIR
描述記憶體規劃：
1. cluster spm bank allocation
2. pe local allocation
3. base addresses
4. AGU iter/stride/tag/mask config

### 5.5 ProgramIR
描述後端程式輸出：
1. template id
2. symbol table for template variables
3. asm text
4. machine code words

## 6. Timing Cost Model 規格

本節定義「使用者需填寫」的參數與用途。建議放在 `hw_constraint.json`。

### 6.1 必填參數清單

#### A. Compute
1. `PE_ARRAY_ROWS`
2. `PE_ARRAY_COLS`
3. `MAC_PER_PE`
4. `CYCLE_PER_MAC`
5. `PIPELINE_DEPTH`
6. `PE_UTILIZATION_FACTOR`
7. `VECTOR_WIDTH_BITS`

#### B. Memory Capacity
1. `CLUSTER_SPM_BANK_NUM`
2. `CLUSTER_SPM_BANK_SIZE_KB`
3. `CLUSTER_SPM_BANK_READ_PORT`
4. `CLUSTER_SPM_BANK_WRITE_PORT`
5. `PE_LOCAL_MEM_SIZE_KB`
6. `PE_LOCAL_READ_PORT`
7. `PE_LOCAL_WRITE_PORT`

#### C. Bandwidth
1. `BW_DRAM_TO_SPM_BPC` (bytes per cycle)
2. `BW_SPM_TO_PE_BPC`
3. `BW_PE_TO_SPM_BPC`
4. `BW_SPM_TO_NOC_BPC`
5. `BW_NOC_TO_SPM_BPC`

#### D. NoC
1. `NOC_FLIT_SIZE_BYTES`
2. `NOC_HOP_LATENCY`
3. `NOC_ROUTER_PIPE_STAGES`
4. `NOC_LINK_BW_BPC`
5. `NOC_MULTICAST_SUPPORT`
6. `NOC_CONTENTION_ALPHA`

#### E. Overhead
1. `DMA_SETUP_LATENCY`
2. `DMA_TEARDOWN_LATENCY`
3. `CONTEXT_SWITCH_CYCLES`
4. `PE_PIPELINE_STALL_PENALTY`
5. `BANK_CONFLICT_PENALTY`

#### F. Precision and Data Format
1. `ACT_BYTES`
2. `WEIGHT_BYTES`
3. `ACC_BYTES`
4. `OUTPUT_BYTES`

### 6.2 推薦選填參數
1. `THERMAL_THROTTLE_FACTOR`
2. `DVFS_LEVEL`
3. `DMA_BURST_BYTES`
4. `CACHELINE_BYTES` (若有 shared cache)

### 6.3 成本函數建議

以每個候選 schedule 的總延遲估算：

$$
T_{total} = \sum_{wave\in W} \left( \max(T_{compute}, T_{read}, T_{write}) + T_{switch} + T_{penalty} \right)
$$

其中：

$$
T_{compute} = \frac{Ops_{wave}}{PE_{rows}\cdot PE_{cols}\cdot MAC_{perPE}\cdot Util}\cdot Cycle_{mac}
$$

$$
T_{read} = \frac{Bytes_{ifmap}+Bytes_{weight}+Bytes_{bias}}{BW_{effective\_in}} + Lat_{dma}
$$

$$
T_{write} = \frac{Bytes_{ofmap}}{BW_{effective\_out}} + Lat_{dma}
$$

$$
T_{penalty} = P_{bank} + P_{noc\_contention} + P_{pipeline\_bubble}
$$

建議同時支援多目標分數：

$$
Score = w_t\cdot T_{total} + w_e\cdot E_{est} + w_m\cdot MemFrag
$$

## 7. hw_constraint.json 範例模板

```json
{
    "version": "1.0",
    "compute": {
        "PE_ARRAY_ROWS": 12,
        "PE_ARRAY_COLS": 8,
        "MAC_PER_PE": 32,
        "CYCLE_PER_MAC": 1,
        "PIPELINE_DEPTH": 4,
        "PE_UTILIZATION_FACTOR": 0.88,
        "VECTOR_WIDTH_BITS": 256
    },
    "memory": {
        "CLUSTER_SPM_BANK_NUM": 16,
        "CLUSTER_SPM_BANK_SIZE_KB": 64,
        "CLUSTER_SPM_BANK_READ_PORT": 1,
        "CLUSTER_SPM_BANK_WRITE_PORT": 1,
        "PE_LOCAL_MEM_SIZE_KB": 8,
        "PE_LOCAL_READ_PORT": 1,
        "PE_LOCAL_WRITE_PORT": 1
    },
    "bandwidth": {
        "BW_DRAM_TO_SPM_BPC": 64,
        "BW_SPM_TO_PE_BPC": 16,
        "BW_PE_TO_SPM_BPC": 16,
        "BW_SPM_TO_NOC_BPC": 32,
        "BW_NOC_TO_SPM_BPC": 32
    },
    "noc": {
        "NOC_FLIT_SIZE_BYTES": 16,
        "NOC_HOP_LATENCY": 2,
        "NOC_ROUTER_PIPE_STAGES": 2,
        "NOC_LINK_BW_BPC": 16,
        "NOC_MULTICAST_SUPPORT": true,
        "NOC_CONTENTION_ALPHA": 1.15
    },
    "overhead": {
        "DMA_SETUP_LATENCY": 40,
        "DMA_TEARDOWN_LATENCY": 10,
        "CONTEXT_SWITCH_CYCLES": 20,
        "PE_PIPELINE_STALL_PENALTY": 8,
        "BANK_CONFLICT_PENALTY": 12
    },
    "dtype": {
        "ACT_BYTES": 2,
        "WEIGHT_BYTES": 2,
        "ACC_BYTES": 4,
        "OUTPUT_BYTES": 2
    }
}
```

## 8. 前端輸入格式 (op json)

### 8.1 統一格式

```json
{
    "version": "1.0",
    "name": "layer_004",
    "operation": "conv2d_3x3",
    "dtype": "int16",
    "input": {
        "N": 1,
        "C": 64,
        "H": 224,
        "W": 224
    },
    "weight": {
        "K": 128,
        "C": 64,
        "R": 3,
        "S": 3
    },
    "output": {
        "N": 1,
        "K": 128,
        "H": 224,
        "W": 224
    },
    "attributes": {
        "stride_h": 1,
        "stride_w": 1,
        "pad_h": 1,
        "pad_w": 1,
        "dilation_h": 1,
        "dilation_w": 1,
        "groups": 1,
        "allow_ultra_mode": true
    },
    "policy": {
        "opt_goal": "latency",
        "max_workspace_kb": 4096,
        "deterministic": true
    }
}
```

### 8.2 GEMM 例子

```json
{
    "version": "1.0",
    "name": "fc_01",
    "operation": "gemm",
    "dtype": "int16",
    "gemm": {
        "M": 1024,
        "N": 1024,
        "K": 2048,
        "transA": false,
        "transB": false
    },
    "policy": {
        "opt_goal": "latency",
        "allow_ultra_mode": true
    }
}
```

## 9. PE Template 設計規範

### 9.1 為何不讓前端直接寫 asm
1. 高機率出現 loop count 與 tile 不一致。
2. 不同 kernel 的地址生成規則複雜，前端難保證正確。
3. 模板可由後端集中驗證與回歸測試。

### 9.2 Template 變數命名規範
1. 與現有 pe-isa assembler 對齊，模板變數使用 `$(NAME)`。
2. 必須全部大寫蛇形命名。
3. 未解析變數視為編譯錯誤。
4. 若前端仍輸出 `{{ NAME }}`，需在 backend 做一次語法轉換後再餵 assembler。

常用變數：
1. `$(LOOP_OUTER_0)`, `$(LOOP_OUTER_1)`
2. `$(LOOP_INNER_K)`
3. `$(BASE_IFMAP)`, `$(BASE_WEIGHT)`, `$(BASE_OFMAP)`
4. `$(STRIDE_IFMAP_0)`, `$(STRIDE_WEIGHT_0)`, `$(STRIDE_OFMAP_0)`
5. `$(ULTRA_MODE)`

### 9.3 template asm 範例 (參考現有 pe-isa template)

以下範例對齊 `design/hybridacc-pe-isa/asm/template/gemv_template.asm` 的語法風格：
1. 使用 `.template` 區塊
2. 以 `template_name(ARG=default, ...)` 定義參數
3. 指令內以 `$(ARG)` 取代

```asm
## gemm.template.asm (cluster compiler backend managed)

.template
gemm_template(
    KERNEL_DMA_STORE_LEN=64,
    KERNEL_DMA_LOAD_LEN=256,
    INPUT_DIM=32,
    OUTPUT_DIM=8,
    PSUM_COUNT=24,
    TILE_LOOP=1
):
    # Partial-sum register init
    CLEAR.P

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_STORE_LEN)
    SDMA.LOOP 1
    SDMA.SD 4

loop_tile:
    LOOPIN $(TILE_LOOP)
    SWAPDM

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LOAD_LEN)
    LDMA.LHB 1

loop_in_dim:
    LOOPIN $(INPUT_DIM)

    # Feed input vectors to tensor registers
    TSTORE t0
    TSTORE t3
    TSTORE t6
    TSTORE t9
    TSTORE t1
    TSTORE t4
    TSTORE t7
    TSTORE t10
    TSTORE t2
    TSTORE t5
    TSTORE t8
    TSTORE t11

    SETRID.PT 0, 0
compute_gemm:
    LOOPIN $(OUTPUT_DIM)
    VMULR 1, 1
    VMULR 1, 1
    VMULRN 1, VTRST
    LOOPEND

    SETRID.P 0
    LOOPEND

psum_reduce:
    LOOPIN $(PSUM_COUNT)
    VPSUMR 1
    LOOPEND
    CLEAR.P

    LOOPEND
    HALT
```

建議 backend 做兩層檢查：
1. registry `required_vars` 是否全部有綁定值。
2. 展開後 asm 是否仍存在 `$(...)` 未解析符號。

### 9.4 Kernel Registry

`kernels/registry.yaml` 建議格式：

```yaml
version: 1
kernels:
    conv2d_1x1:
        template: conv2d_1x1.template.asm
        assembler: pe_asm_v1
        required_vars:
            - LOOP_OUTER_0
            - LOOP_OUTER_1
            - LOOP_INNER_K
            - BASE_IFMAP
            - BASE_WEIGHT
            - BASE_OFMAP
    conv2d_3x3:
        template: conv2d_3x3.template.asm
        assembler: pe_asm_v1
        required_vars:
            - LOOP_OUTER_0
            - LOOP_OUTER_1
            - LOOP_INNER_K
    gemm:
        template: gemm.template.asm
        assembler: pe_asm_v1
        required_vars:
            - LOOP_M
            - LOOP_N
            - LOOP_K
```

## 10. 避免 per-wave 爆量的資料壓縮策略

### 10.1 Loop Descriptor
每個 operation 輸出 loop descriptor，而非展平所有 wave 明細：

```json
{
    "loop_descriptor": {
        "dims": [64, 32, 4],
        "names": ["tile_h", "tile_w", "tile_k"],
        "ifmap_stride": [512, 16, 2],
        "weight_stride": [0, 64, 2],
        "ofmap_stride": [512, 16, 0]
    }
}
```

建議補充「如何由 dims 重建 wave 順序」的規則：
1. `dims` 與 `names` 需同長度，且 index 對齊。
2. 最右維視為最快變動維度（inner-most loop）。
3. wave id 以 row-major 方式遞增。

Conv2D 例子（`tile_h=2, tile_w=3, tile_k=2`，共 12 waves）：

```json
{
    "loop_descriptor": {
        "dims": [2, 3, 2],
        "names": ["tile_h", "tile_w", "tile_k"],
        "ifmap_stride": [4096, 512, 64],
        "weight_stride": [0, 0, 128],
        "ofmap_stride": [2048, 256, 0]
    }
}
```

對應 wave id 展開（前 6 筆示意）：
1. `wave_id=0` -> `(tile_h=0, tile_w=0, tile_k=0)`
2. `wave_id=1` -> `(tile_h=0, tile_w=0, tile_k=1)`
3. `wave_id=2` -> `(tile_h=0, tile_w=1, tile_k=0)`
4. `wave_id=3` -> `(tile_h=0, tile_w=1, tile_k=1)`
5. `wave_id=4` -> `(tile_h=0, tile_w=2, tile_k=0)`
6. `wave_id=5` -> `(tile_h=0, tile_w=2, tile_k=1)`

GEMM 例子（`tile_m=2, tile_n=2, tile_k=4`，共 16 waves）：

```json
{
    "loop_descriptor": {
        "dims": [2, 2, 4],
        "names": ["tile_m", "tile_n", "tile_k"],
        "ifmap_stride": [8192, 0, 128],
        "weight_stride": [0, 8192, 128],
        "ofmap_stride": [4096, 2048, 0]
    }
}
```

其中 `tile_k` 為 inner-most，可保證 K-split 累加時，連續 wave 使用相同 `(tile_m, tile_n)`。

### 10.2 壓縮分層設計 (建議正式採用)

為了同時兼顧可重建性與檔案大小，Stage0 建議分成三層：
1. `wave_schedule`：只描述波次數量與切分維度，不展開每顆 PE。
2. `spm.tensor_mapping`：描述 tensor 到 SPM group/section 的靜態映射。
3. `dma.waves[]` + `cluster_plans[]`：只保存每 wave 必要差異欄位。

對應目前 `cluster_gen.py` 的做法：
1. 先由 `_build_spm_dma_plan(...)` 生成 `spm` 與 `dma`。
2. 再由 `_compile_cluster_plans_conv2d(...)` 或 `_compile_cluster_plans_gemm(...)` 生成 `cluster_plans`。

可操作範例（Conv2D，`temporal_wave_count=4`）：

```json
{
    "software": {
        "wave_schedule": {
            "temporal_wave_count": 4,
            "temporal_wave_out_h": 1,
            "temporal_wave_out_ch": 1,
            "temporal_wave_in_ch": 4
        },
        "spm": {
            "groups": [
                {
                    "group_id": 0,
                    "sections": [
                        {"name": "g0_ping", "global_linear_addr": 0},
                        {"name": "g0_pong", "global_linear_addr": 65536}
                    ]
                },
                {
                    "group_id": 1,
                    "sections": [
                        {"name": "g1_ping", "global_linear_addr": 131072},
                        {"name": "g1_pong", "global_linear_addr": 196608}
                    ]
                }
            ],
            "tensor_mapping": {
                "weight": {
                    "group_id": 0,
                    "per_wave_words64": [144, 144, 144, 144]
                },
                "activation": {
                    "group_id": 1,
                    "per_wave_words64": [320, 320, 320, 320]
                }
            }
        },
        "dma": {
            "waves": [
                {
                    "wave_id": 0,
                    "runtime_sections": {"weight": "g0_ping", "activation": "g1_ping"}
                },
                {
                    "wave_id": 1,
                    "runtime_sections": {"weight": "g0_pong", "activation": "g1_ping"}
                },
                {
                    "wave_id": 2,
                    "runtime_sections": {"weight": "g0_ping", "activation": "g1_pong"}
                },
                {
                    "wave_id": 3,
                    "runtime_sections": {"weight": "g0_pong", "activation": "g1_pong"}
                }
            ]
        },
        "cluster_plans": [
            {"wave_id": 0, "agu_ps": {"base_addr": 0, "iter0": 4, "stride0": 1}},
            {"wave_id": 1, "agu_ps": {"base_addr": 576, "iter0": 4, "stride0": 1}},
            {"wave_id": 2, "agu_ps": {"base_addr": 1152, "iter0": 4, "stride0": 1}},
            {"wave_id": 3, "agu_ps": {"base_addr": 1728, "iter0": 4, "stride0": 1}}
        ]
    }
}
```

上述範例的重建邏輯：
1. 先用 `wave_schedule` 確認總 wave 數與切分軸。
2. 對每個 `wave_id`，由 `dma.waves[wave_id].runtime_sections` 找到 section 名稱。
3. 用 section 名稱回查 `spm.groups[].sections[]` 取得實際 SPM 位址。
4. 用 `tensor_mapping.<tensor>.per_wave_words64[wave_id]` 補上本 wave 傳輸長度。
5. 用 `cluster_plans[wave_id].agu_*` 還原 compute 端地址生成參數。

GEMM 壓縮對照例（K-split，`temporal_wave_count=3`）：

```json
{
    "software": {
        "wave_schedule": {
            "temporal_wave_count": 3,
            "temporal_wave_m": 1,
            "temporal_wave_n": 1,
            "temporal_wave_k": 3
        },
        "spm": {
            "tensor_mapping": {
                "activation": {"per_wave_words64": [256, 256, 256]},
                "weight": {"per_wave_words64": [256, 256, 256]},
                "partial_sum": {"per_wave_words64": [128, 0, 0]},
                "output": {"per_wave_words64": [0, 0, 128]}
            }
        }
    }
}
```

此例表示：
1. wave 0 先載入 partial sum。
2. wave 1 僅做中間累加，不額外載入/回寫 partial sum。
3. wave 2 才輸出最終 output。

### 10.3 必保留欄位與可省略欄位

必保留：
1. `wave_schedule.temporal_wave_count`
2. `spm.groups[]` 與 `spm.tensor_mapping`
3. `dma.waves[].runtime_sections`
4. `cluster_plans[].agu_*` 的 `base_addr/iter*/stride*/tag*`

可省略或延後輸出：
1. 不必要的每 PE 展平路由細節（可由 scan chain + route rule 重建）
2. 每次 wave 完整 tensor shape 重複描述（保留在 `meta.tensor_shapes` 即可）
3. 大量重複為 0 的欄位（可在 stage1 pack 時補預設值）

### 10.4 `per_wave_words64` 壓縮策略

`tensor_mapping.<tensor>.per_wave_words64` 用於描述每 wave DMA 資料量，取代巨量 transfer 展平。

Conv2D 建議規則：
1. `activation`：依 `(out_h_wave, in_ch_wave)` 分配，若同時切 OCH/ICH 可增量重載。
2. `weight`：依 `(out_ch_wave, in_ch_wave)` 分配。
3. `partial_sum`：通常在第一個 `in_ch_wave` 載入。
4. `output`：通常在最後一個 `in_ch_wave` 回寫。

GEMM 建議規則：
1. `weight`/`activation` 依 `K` 或 `(M,N,K)` wave 切分。
2. `partial_sum` 與 `output` 依是否 K-split 決定是每 wave 都有，或僅首尾 wave 存取。

### 10.5 Runtime Section 參照壓縮

每個 wave 不直接重複完整位址，而是引用 section 名稱：
1. `runtime_sections.activation = "g1_ping"`
2. `runtime_sections.weight = "g0_b0_ping"`

實際位址可由：
1. `spm.groups[].sections[].global_linear_addr` / `global_parallel_addr`
2. `spm.tensor_mapping.<tensor>.spm_mode`
重建。

### 10.6 Residual Tile
尾塊不整除時可選策略：
1. `pad_and_mask`
2. `separate_residual_kernel`
3. `fallback_scalar_epilogue`

建議預設 `pad_and_mask`，在 stage0 中保存：
1. `mask_cfg`（AGU mask）
2. residual 的 `iter/stride` 修正
3. residual 專用 schedule id（便於除錯）

## 11. Stage 0 輸出格式 (Debug JSON)

### 11.1 設計原則
1. 可讀性高
2. 可完整回放到模擬器
3. 可由工具轉 stage1
4. 與目前 `cluster_gen.py` 實際輸出欄位相容

### 11.2 建議結構 (對齊現有 cluster_gen)

Stage0 建議維持三層主結構：
1. `meta`
2. `hardware`
3. `software`

其中 `software` 再拆為：
1. `files` / `dram_mapping`
2. `wave_schedule`
3. `spm`
4. `dma`
5. `cluster_plans`

### 11.3 建議 JSON 範例

```json
{
    "version": "1.0",
    "meta": {
        "name": "conv2d_custom",
        "mode": "conv2d",
        "ultra_mode": false,
        "seed": 123,
        "tensor_shapes": {
            "activation": [1, 16, 200, 16],
            "weight": [16, 3, 3, 16],
            "partial_sum": [1, 14, 198, 16],
            "output": [1, 14, 198, 16]
        },
        "kernel_size": [3, 3],
        "stride": 1,
        "padding": 0
    },
    "hardware": {
        "num_pes": 48,
        "num_bus": 3,
        "spm_bank_size": 8192,
        "spm_groups": 4
    },
    "software": {
        "files": {
            "activation": "input_activation.bin",
            "weight": "input_weight.bin",
            "partial_sum": "input_partial_sum.bin",
            "output": "output_partial_sum.bin",
            "pe_program": "pe_program.bin"
        },
        "dram_mapping": {
            "activation": 0,
            "weight": 268435456,
            "partial_sum": 536870912,
            "output": 805306368,
            "pe_program": 1073741824
        }
    }
}
```

### 11.4 `software.wave_schedule` 規範

```json
{
    "wave_schedule": {
        "temporal_wave_count": 4,
        "temporal_wave_out_h": 1,
        "temporal_wave_out_ch": 1,
        "temporal_wave_in_ch": 4
    }
}
```

### 11.5 `software.spm` 規範

`spm` 建議至少包含：
1. `topology`：群組與 bank 幾何
2. `groups[]`：section 位址表
3. `tensor_mapping`：tensor 到 section 的映射與 `per_wave_words64`

```json
{
    "spm": {
        "base_addr": 0,
        "addr_unit": "byte",
        "topology": {
            "num_groups": 4,
            "banks_per_group": 3,
            "bank_depth_words": 8192
        },
        "groups": [
            {
                "group_id": 0,
                "noc_channel": "PS",
                "sections": [
                    {
                        "name": "g0_ping",
                        "tag": "ping",
                        "local_linear_base": 0,
                        "global_linear_addr": 0,
                        "size_words64_linear": 12288
                    }
                ]
            }
        ],
        "tensor_mapping": {
            "weight": {
                "group_id": 0,
                "section_mode": "group",
                "spm_mode": "linear",
                "spm_addr": 0,
                "size_words64": 576,
                "per_wave_words64": [144, 144, 144, 144]
            }
        }
    }
}
```

### 11.6 `software.dma` 規範

`dma.waves[]` 每筆對應一個 temporal wave，最少欄位：
1. `wave_id`
2. `runtime_sections`
3. `transfers[]`（每個 tensor 的 DRAM<->SPM 行為）

```json
{
    "dma": {
        "engine": "tb_single_sc_process",
        "waves": [
            {
                "wave_id": 0,
                "runtime_sections": {
                    "activation": "g1_ping",
                    "weight": "g0_ping",
                    "partial_sum": "g2_ping",
                    "output": "g3_pong"
                },
                "transfers": [
                    {
                        "tensor": "activation",
                        "direction": "dram_to_spm",
                        "src_dram_addr": 0,
                        "dst_spm_addr": 262144,
                        "size_words64": 3200,
                        "src_addr_gen": {
                            "base_addr": 0,
                            "iter": [1, 16, 200, 1],
                            "stride": [102400, 6400, 32, 8],
                            "unit": "byte"
                        },
                        "dst_addr_gen": {
                            "base_addr": 262144,
                            "iter": [1, 1, 3200, 1],
                            "stride": [25600, 25600, 8, 8],
                            "unit": "byte"
                        }
                    }
                ]
            }
        ]
    }
}
```

### 11.7 `software.cluster_plans` 規範

`cluster_plans[]` 是 compute side 的計畫，與 `dma.waves[]` 透過 wave index 對齊。

```json
{
    "cluster_plans": [
        {
            "name": "CONV_H0_OC0_IC0",
            "global_mask": 15,
            "ultra_mode": false,
            "agu_ps": {
                "base_addr": 0,
                "iter0": 4,
                "iter1": 3,
                "iter2": 3,
                "iter3": 16,
                "stride0": 1,
                "stride1": 4,
                "stride2": 12,
                "stride3": 36,
                "lane_cfg": 0,
                "tag_base": 0,
                "tag_stride0": 1,
                "tag_stride1": 1,
                "tag_ctrl": 2,
                "mask_cfg": 15,
                "ultra": false,
                "enable": true
            }
        }
    ]
}
```

### 11.8 與 Stage1 轉換關係

`stage0 -> stage1` 建議轉換規則：
1. `software.files.pe_program` + assembler metadata 轉 `.text.pe`。
2. `software.cluster_plans[].agu_*` 打包成 `.cfg.agu`。
3. `scan_chain` 與 route mode 打包成 `.cfg.noc`。
4. `software.dma.waves[]` 打包成 `.cfg.mmio` 或 runtime command table。
5. `meta/hardware` 摘要寫入 `.meta`，附版本與 checksum。

### 11.9 `scan_chain` 與 Stage0 的關係

對齊目前 `cluster_gen.py` 測試流程：
1. `scan_chain` 主要輸出為 `scan_chain.txt`/`scan_chain.bin`（獨立於 `config.json`）。
2. Stage0 正式版可採兩種策略：
    - A. 維持外部檔案（與現況相容）
    - B. 內嵌 `software.noc.scan_chain[]`（單檔可回放）
3. 若採 A，需在 Stage0 的 `meta` 內增加 `scan_chain_file` 指標，避免遺漏載入。

## 12. Stage 1 輸出格式 (Runtime Loadable)

### 12.1 建議 ELF Section
1. `.text.pe` PE machine code
2. `.cfg.mmio` MMIO write script
3. `.cfg.agu` AGU table
4. `.cfg.noc` NoC scan chain table
5. `.meta` build info and checksum

### 12.2 載入流程
1. runtime loader 解析 `.cfg.*`
2. 先下發 memory map 與 AGU/NOC
3. 再寫入 PE IMEM
4. 最後送 start command

## 13. CLI 與使用流程

### 13.1 典型命令

```bash
cluster-compiler compile \
    --op testbench/cluster/config.json \
    --hw testbench/cluster/hw_constraint.json \
    --kernel-registry kernels/registry.yaml \
    --out build/layer_004
```

### 13.2 產物
1. `build/layer_004.stage0.json`
2. `build/layer_004.pe.asm`
3. `build/layer_004.pe.hex`
4. `build/layer_004.elf`
5. `build/layer_004.report.txt`

### 13.3 報告內容
1. top-5 schedule candidates
2. 每個 candidate 的 estimated cycles
3. memory pressure 與 bank conflict 指標
4. 被淘汰原因

## 14. Golden Tensor Tool 規格

### 14.1 功能
1. 產生隨機或固定種子張量
2. 以 PyTorch 計算 golden output
3. 輸出二進位與對照 metadata

### 14.2 建議命令

```bash
python tools/generate_golden.py \
    --op testbench/cluster/config.json \
    --out build/golden/layer_004 \
    --seed 1234
```

### 14.3 建議輸出檔
1. `ifmap.bin`
2. `weight.bin`
3. `bias.bin` (optional)
4. `golden_ofmap.bin`
5. `tensor_meta.json`

### 14.4 比對工具

```bash
python tools/compare_tensor.py \
    --golden build/golden/layer_004/golden_ofmap.bin \
    --actual output/cluster/layer_004_ofmap.bin \
    --dtype int16 \
    --atol 0 --rtol 0
```

## 15. 錯誤處理與診斷

### 15.1 編譯期錯誤類型
1. `E1001_INVALID_SHAPE`
2. `E2001_TEMPLATE_VAR_MISSING`
3. `E3001_MEMORY_OVERFLOW`
4. `E3002_BANK_CONFLICT_UNRESOLVED`
5. `E4001_ASSEMBLER_FAILED`

### 15.2 建議錯誤訊息格式

```text
[E2001] template variable missing: LOOP_INNER_K
kernel=conv2d_3x3.template.asm
hint=check schedule lowering and symbol binding
```

## 16. 驗證與評分建議 (提供 Gemini/Reviewer)

### 16.1 文件完整性評分維度
1. 架構完整度
2. 格式可操作性
3. 參數可填寫性
4. 可實作性
5. 可驗證性

### 16.2 最小可行驗證清單
1. conv2d_1x1 小尺寸 smoke test
2. conv2d_3x3 常見尺寸 regression
3. gemm 多組 M/N/K regression
4. ultra mode on/off A/B test
5. stage0 轉 stage1 可回放測試

## 17. 版本策略

1. `op json`, `stage0`, `hw_constraint` 全部帶 `version`
2. major 版不相容升級
3. compiler 保留最多兩代向後解析

## 18. 實作 Roadmap

### Milestone 1: 基礎可用
1. Frontend parser + schema validator
2. 單一 cost model
3. conv2d_1x1/gemm 模板生成
4. stage0 輸出

### Milestone 2: 完整 DSE
1. 多候選 tiling 搜尋
2. bank conflict 感知 mapping
3. report 排名與淘汰理由

### Milestone 3: 量產化
1. stage1 ELF 輸出
2. 全 kernel 模板覆蓋
3. golden tool CI 驗證

## 19. 結論

本規劃採用「前端高階描述 + 後端模板化程式生成 + 迴圈描述壓縮」的架構，能同時解決：
1. config 與 asm 分離難維護
2. per-wave 資料爆量
3. cost model 尚未落地時缺少可填寫規範

依本文件落地後，團隊可先由 `hw_constraint.json` 建立成本參數，再用同一套流程產出 stage0/stage1，最後用 golden tool 建立自動回歸，形成完整可迭代的 Cluster Compiler 開發閉環。
