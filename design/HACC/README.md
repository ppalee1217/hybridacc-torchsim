# HACC Python Package

HACC 已從原本的 C++ 子專案遷移為 Python package `hacc`。

## 位置

- Python package: `design/HACC/hacc`
- 測試: `design/HACC/test/test_compiler.py`
- 規格文件: `design/HACC/doc/HACC.md`
- CLI 輸入規格: `design/HACC/input_spec.md`

## 安裝

在 repository root 執行：

```bash
pip install -e .
```

## CLI

編譯 HACC spec 檔：

```bash
hacc compile path/to/op.yaml -o /tmp/output_prefix
```

範例：

```bash
hacc compile design/HACC/examples/conv2d_basic.yaml -o /tmp/conv2d_basic
hacc compile design/HACC/examples/gemm_basic.yaml -o /tmp/gemm_basic
```

完整欄位定義、預設值與 YAML 範例請見：

- `design/HACC/input_spec.md`

檢視輸出的 HACC ELF：

```bash
hacc-elfdump -d /tmp/output_prefix.hacc.elf
```

## 端到端教學

以下示範從輸入 spec 編譯到檢視 ELF 內容的完整流程。

### 1. 使用範例 spec 編譯

Conv2D：

```bash
hacc compile design/HACC/examples/conv2d_basic.yaml -o /tmp/conv2d_basic
```

GEMM：

```bash
hacc compile design/HACC/examples/gemm_basic.yaml -o /tmp/gemm_basic
```

成功後，CLI 會輸出類似：

```text
compiled conv2d_basic -> /tmp/conv2d_basic.hacc.elf
compiled gemm_basic -> /tmp/gemm_basic.hacc.elf
```

### 2. 確認產物

每次成功編譯都會產生兩個主要輸出：

1. `<output_prefix>.hacc.elf`
2. `<output_prefix>.debug.json`

例如：

```bash
ls /tmp/conv2d_basic.hacc.elf /tmp/conv2d_basic.debug.json
```

### 3. 用 elfdump 看 section 與 MCU 韌體反組譯

只看 section header 與 `.hacc.core` 反組譯：

```bash
hacc-elfdump -d /tmp/conv2d_basic.hacc.elf
```

若還想同時看 raw bytes hex dump：

```bash
hacc-elfdump -d -x /tmp/conv2d_basic.hacc.elf
```

實際輸出會包含：

```text
ELF32 header:
	shoff: 1484  shnum: 12  shstrndx: 11

Section headers:
	[1] .hacc.core offset=52 size=244
	00000000: 04400000    movi    r1, 0
	00000004: 08400400    movhi   r1, 0x400
	...
	[2] .hacc.job offset=296 size=76
	[3] .hacc.block offset=372 size=76
	[4] .hacc.profile offset=448 size=576
	[5] .hacc.dma offset=1024 size=120
	[6] .hacc.agu offset=1144 size=168
	[8] .hacc.pe offset=1312 size=40
	[9] .hacc.scan offset=1352 size=12
```

### 4. 你通常會檢查什麼

1. `.hacc.core` 是否存在，且能正常反組譯出 MCU 指令。
2. `.hacc.job`、`.hacc.block`、`.hacc.profile`、`.hacc.dma`、`.hacc.agu` 等 section 是否都有合理大小。
3. `.hacc.nlu`、`.hacc.patch` 在某些 case 可能是 0 bytes；這不一定代表錯誤，要看輸入 op 是否真的需要它們。
4. `.debug.json` 可用來交叉比對 block、wave 與 section 內容，做 smoke test 與 regression 檢查。

### 5. 怎麼讀 `.debug.json`

`<output_prefix>.debug.json` 是 compiler 在 stage0 輸出的摘要檔，目的是讓你不用先解 ELF binary，就能快速確認這次編譯的 shape、loop、cluster 與 section 大小是否合理。

實際範例：

```json
{
  "stage": "stage0",
  "op_name": "conv2d_basic",
  "op_type": 0,
  "input_shape": [1, 16, 8, 8],
  "output_shape": [1, 32, 8, 8],
  "loop_extents": [1, 3, 2, 1],
  "loop_names": ["tile_oh", "tile_oc", "tile_ic", "unused"],
  "total_waves": 6,
  "num_clusters": 3,
  "cluster_mask": 7,
  "sections": {
	 "core_words": 61,
	 "job_words": 19,
	 "block_words": 19,
	 "profile_words": 144,
	 "dma_words": 30,
	 "agu_words": 42,
	 "nlu_words": 0,
	 "pe_words": 10,
	 "scan_words": 3,
	 "patch_words": 0
  },
  "blocks": [
	 {
		"loop_rank": 2,
		"loop_extent": [1, 3, 2, 1],
		"cluster_mask": 7,
		"rule_stride": 6,
		"total_waves": 6,
		"patch_count": 0,
		"flags": 6
	 }
  ],
  "patch_count": 0,
  "nlu_rule_count": 0
}
```

建議閱讀順序：

1. `op_name` / `op_type`
	- 先確認這份 debug 檔對應的是哪個 op。
	- 目前 `op_type=0` 代表 Conv2D，`op_type=1` 代表 GEMM。

2. `input_shape` / `output_shape`
	- 用來快速確認 frontend 的 shape 推導是否符合預期。
	- 對 Conv2D，這通常是最先檢查 padding / stride 是否填錯的地方。

3. `loop_extents` / `loop_names` / `total_waves`
	- 這組欄位描述 planner 後的 loop 結構。
	- `total_waves` 應和 loop extents 的乘積一致。
	- 若 wave 數遠大於預期，通常代表 tile 切法太碎或某個維度估錯。

4. `num_clusters` / `cluster_mask`
	- 用來確認 scheduler 最後用了幾個 cluster。
	- `cluster_mask` 可直接和 runtime section 的 cluster fanout 對照。

5. `sections`
	- 這是每個 `.hacc.*` section 的 word 數摘要。
	- 最常用來做 smoke test：確認 `core_words`、`block_words`、`profile_words`、`dma_words` 是否非零。
	- `nlu_words`、`patch_words` 為零不一定有問題，要看輸入 op 是否真的需要 NLU phase 或 residual patch。

6. `blocks`
	- 每個元素代表一個壓縮 block 摘要。
	- 可用來檢查 `loop_rank`、`loop_extent`、`cluster_mask`、`rule_stride`、`patch_count` 是否和 block builder 預期一致。
	- 如果 block 數異常增加，通常代表壓縮失效或 patch 過多。

7. `patch_count` / `nlu_rule_count`
	- 這兩個是全域摘要。
	- 適合拿來做 regression 檢查，例如某次修改後 patch 數突然暴增。

實務上最常見的交叉比對方式：

1. 用 `.debug.json` 看 `sections.core_words`、`sections.block_words`、`sections.profile_words`。
2. 再用 `hacc-elfdump -d` 確認 `.hacc.core` 與各 section header 大小是否一致。
3. 若結果不符，優先檢查 payload builder 或 ELF packager。

## 測試

```bash
python design/HACC/test/test_compiler.py
```

## 模組對照

- `frontend`：建立與驗證 Conv2D / GEMM `OpIR`
- `planner`：決定 tiling、loop extents 與 cluster mask
- `cluster_lowering`：產生 PE / scan / AGU / profile
- `nlu_lowering`：產生 NLU rules
- `block_builder`：建立壓縮 block 與 residual patch
- `payload_builder`：序列化各 section table
- `firmware_emitter`：產生 `.hacc.core` 指令字
- `elf_packager`：寫出 `.hacc.*` ELF 與 stage0 JSON
- `utils` / `elfdump`：提供 ELF dump 與反組譯工具