# HybridAcc Core-ISA 與 Program Packaging 規格（Cluster + DMA 協同）

> 目標：定義一套可擴充到多個 cluster 的 control ISA，負責 cluster MMIO 設定、狀態輪詢/中斷、DMA 搬運，以及把 readable text program 編譯成可載入區段（header / pe-program / core-program / scan-chain）。

---

## 0. 設計目標與邊界

### 0.1 需求背景

目前你已可控制「單一 wave」的 cluster 行為，但要達成連續 wave 的高吞吐執行，必須讓 **Core Controller** 同步做兩件事：
1. 控制 cluster MMIO（SPM config / HDDU AGU / NoC command / PE 啟停）
2. 控制 DMA MMIO（input/weight/output 的搬運、雙 buffer）

### 0.2 設計原則

1. **可擴充到 N 個 cluster**：程式必須明確標註部署 cluster mask/id。
2. **將資料搬運與計算解耦**：DMA 與 cluster 可交錯執行（pipeline wave）。
3. **與現有 MMIO map 對齊**：沿用 `ComputeCluster/HDDU` 文件地址空間。
4. **可編譯、可追蹤**：readable text -> IR -> binary sections，保留可除錯 metadata。

---

## 1. Core-ISA 設計

本章定義執行在 Core Controller 的指令集（不是 PE ALU ISA）。

## 1.1 執行模型

- 單一 core thread，in-order 執行。
- 32-bit 指令寬度（建議），64-bit immediate 由 EXT 指令擴展。
- 程式可對「一組 cluster」做同構操作（broadcast + mask）。
- 以 memory-mapped bus 方式訪問：
	- Cluster CMD/MMIO slave（32-bit）
	- Cluster Data slave（64-bit，若 core 直通）
	- DMA MMIO（32-bit）

## 1.2 位址模型（多 Cluster）

為可擴充 cluster 數量，定義分層位址：

```text
Global MMIO Address = CLUSTER_STRIDE * cluster_id + local_mmio_addr
```

建議常數：
- `CLUSTER_STRIDE = 0x0001_0000`
- `cluster_id = 0..(cluster_count-1)`
- local mmio（依 ComputeCluster 定義）
	- `0x0000~0x00FF`: SPM config
	- `0x1000~0x1FFF`: HDDU passthrough
	- `0x2000~0x20FF`: NoC command

DMA 建議獨立 base：
- `DMA_BASE = 0x8000_0000`
- `DMA_CH_STRIDE = 0x100`

## 1.3 寄存器檔（Core ISA）

- `r0..r31`：32-bit 通用暫存器
- `m0..m3`：cluster mask 暫存器（bit i 代表 cluster i）
- `pc`：program counter
- `sr`：status flags（Z/N/C/V + IRQ pending）
- `sp`：stack pointer（建議對應 `r29`）
- `fp`：frame pointer（建議對應 `r30`）
- `lr`：link register（建議對應 `r31`）

### 建議保留暫存器分工

- caller-saved：`r0~r15`
- callee-saved：`r16~r28`
- special：`r29=sp`, `r30=fp`, `r31=lr`

> 第一版若不做完整 ABI，也至少固定 `r31` 為 return address，以支援 `CALL/RET`。

## 1.4 指令類別

### A) 控制流

- `NOP`
- `HALT`
- `JMP label`
- `BEQ rA, rB, label`
- `BNE rA, rB, label`
- `BLT rA, rB, label`
- `CALL label`（`lr <- pc+4`, `pc <- label`）
- `RET`（`pc <- lr`）
- `CALLR rX`（`lr <- pc+4`, `pc <- rX`，可選）

### B) 算術/邏輯（最小集合）

- `MOV rd, rs`
- `LI rd, imm32`
- `ADD rd, ra, rb`
- `ADDI rd, ra, imm16`
- `SUB rd, ra, rb`
- `AND/OR/XOR/SHL/SHR`

### C) MMIO / DMA / Cluster 專用

#### 基本 MMIO
- `MMIO.W addr_reg, data_reg`：寫 32-bit
- `MMIO.R data_reg, addr_reg`：讀 32-bit
- `MMIO.WI addr_imm, imm32`：寫立即值（assembler 展成 LI+MMIO.W 亦可）

#### 多 Cluster 廣播
- `CLMASK.SET mX, imm32`：設定 cluster mask
- `CL.MMIO.W mX, local_addr_imm, data_reg`
- `CL.MMIO.WI mX, local_addr_imm, imm32`
- `CL.MMIO.R.ANY rd, mX, local_addr_imm`：讀第一個有效 cluster
- `CL.MMIO.R.ALL base_rd, mX, local_addr_imm`：依 cluster id 寫入連續暫存區（可選擇性實作）

#### Data Path / DMA
- `DMA.START ch, desc_ptr`
- `DMA.POLL rd, ch, field`（busy/done/error）
- `DMA.WAIT ch`
- `DMA.BARRIER`

#### 同步與事件
- `WAIT.INT mask, timeout_reg`：等待中斷（cluster done/error, dma done/error）
- `WAIT.COND addr_reg, bitmask, expect, timeout_reg`：輪詢某 MMIO 位元
- `FENCE.IO`：確保所有 MMIO write 送達

### D) 批次/巨集執行（建議，對應第 8 章）

- `HDDU.APPLY profile_id`：由硬體 sequencer 展開 profile MMIO 寫入
- `DMA.START.RANGE begin, count`：連續啟動 DMA descriptors
- `DMA.WAIT.MASK ch_mask, timeout_reg`：等待多通道 DMA
- `WAVE.LOAD desc_ptr`：載入 wave descriptor 到內部 shadow registers
- `WAVE.EXEC`：執行當前 wave（prefetch/run/sync/drain）
- `WAVE.NEXT`：`wave_idx++` 並刷新必要動態欄位

## 1.5 建議的 pseudo instruction（提升可讀性）

- `CLUSTER.CFG_MAP mX, map` -> `CL.MMIO.WI mX, 0x0000, map` + `CL.MMIO.WI mX, 0x0004, 1`
- `HDDU.START_ALL mX` -> `CL.MMIO.WI mX, 0x1800, (1<<2)`
- `HDDU.CLEAR_ERR mX` -> `CL.MMIO.WI mX, 0x1800, (1<<1)`
- `PE.START mX` -> `CL.MMIO.WI mX, 0x2000, pack_cmd(CMD_START_PE, 0)`
- `PE.LOAD mX, im_addr, inst16` -> `CL.MMIO.WI mX, 0x2000, pack_load_program(...)`
- `CALL.SAFE label, timeout` -> 寫入 watchdog + `CALL label` + timeout trap
- `WAVE.RUN desc_ptr` -> `WAVE.LOAD desc_ptr` + `WAVE.EXEC` + `WAVE.NEXT`

### 1.5.1 `CALL/RET` 呼叫慣例（建議）

#### Prologue

```asm
ADDI sp, sp, -16
ST   r16, [sp+0]
ST   r17, [sp+4]
ST   fp,  [sp+8]
ST   lr,  [sp+12]
MOV  fp, sp
```

#### Epilogue

```asm
MOV  sp, fp
LD   r16, [sp+0]
LD   r17, [sp+4]
LD   fp,  [sp+8]
LD   lr,  [sp+12]
ADDI sp, sp, 16
RET
```

#### 參數傳遞

- `r0~r3`：前四個參數
- `r4~r5`：回傳值 / status
- 更多參數走 stack

#### 中斷安全建議

- ISR 入口先保存 `r0~r7 + lr + sr`。
- 若 `CALL` 可被中斷打斷，`lr` 必須在 ISR 內保護。

## 1.6 指令編碼建議（32-bit）

```text
[31:26] opcode
[25:21] rd / mask id
[20:16] ra
[15:11] rb
[10:0]  imm11 / ext selector
```

對於 `local_addr_imm` 或 `imm32`：
- 使用 EXT 格式（下一個 word 為 immediate）
- 或 assembler 自動展開多指令

> 實務上，第一版可先用「文字 opcode + assembler 直接輸出微碼格式」，不必立即鎖死硬編碼。

## 1.7 狀態/中斷建議語意

### Cluster 側（由 `HDDU_STATUS`/`interrupt_o`）
- done: `HDDU_STATUS.bit1 = 1`
- err: `HDDU_STATUS.bit2 = 1`

### DMA 側（建議）
- done: `DMA_STATUS.bit0 = 1`
- err: `DMA_STATUS.bit1 = 1`

### Core 程式建議序列
1. `DMA.START` prefetch 下一個 wave
2. 配置本 wave cluster MMIO
3. `PE.START` + `HDDU.START_ALL`
4. `WAIT.INT(cluster_done | dma_done)`
5. 進行下一波 swap buffer

### 1.8 指令延遲與可見性語意（供 compiler 排程）

- `MMIO.W*`：posted write；除非 `FENCE.IO`，不保證立刻對目標可見。
- `MMIO.R*`：具同步語意，會隱含 drain 對同目標的先前 writes。
- `DMA.START*`：只保證 descriptor 被接受，不保證搬運完成。
- `DMA.WAIT*` / `WAIT.*`：blocking 指令，可被中斷喚醒。
- `HDDU.APPLY`：若硬體有 profile cache，命中時延遲固定；miss 時由 sequencer 展開。

---

## 1.9 Pseudo 指令 lowering 規範（assembler/compiler 必備）

### `HDDU.APPLY profile_id`

展開策略：
1. 讀取 `SEC_PROFILE_TABLE[profile_id]`
2. 若 `profile_id == last_profile_id` 且 `dynamic_dirty=0`，可略過
3. 否則依序發出：
	 - global regs：`H_G_CTRL(clear)` -> `PLANE_EN` -> `PLANE_MODE` -> `MAX_OUT`
	 - AGU PS/PD/PLI/PLO（static + dynamic）

### `DMA.START.RANGE begin, count`

```text
for i in [begin, begin+count):
	DMA.START desc[i]
```

### `WAVE.EXEC`

```text
RUN_DMA_PREFETCH_RANGE()
HDDU.APPLY(desc.profile)
START_PE_AND_HDDU()
SYNC_AND_DRAIN(desc.sync_policy)
```

---

---

## 2. PE-ISA 與參數設定的 readable program text file 格式

本章定義單一可讀檔（建議副檔名 `.hacc`），包含：
- PE asm code
- HDDU information
- grouping information
- hardware param
- temporal wave information

## 2.1 檔案整體結構

```text
program <name> {
	meta { ... }
	hardware { ... }
	grouping { ... }
	tensors { ... }
	pe_programs { ... }
	scan_chain { ... }
	hddu_profiles { ... }
	dma_plan { ... }
	temporal_waves { ... }
	core_flow { ... }
}
```

## 2.2 區塊定義（詳細）

### A) `meta`
- `version`
- `target = simulator|fpga`
- `cluster_count`（必要）
- `deploy_mask`（必要，例：`0b0011`）

### B) `hardware`
- `cluster_stride`
- `cluster_mmio_base`
- `dma_base`
- `noc_ports`, `pes_per_port`, `spm_size`, `spm_word_bytes`

### C) `grouping`
- 定義 cluster 分群（同一份程式不同參數）
- 例：`group g0 clusters=[0,1]`，`group g1 clusters=[2,3]`

### D) `tensors`
- 宣告邏輯張量與記憶體位置
- 欄位：`name`, `dtype`, `shape`, `layout`, `spm_base`, `size_bytes`, `dma_src`

### E) `pe_programs`
- 可嵌入 asm 文本或外部檔案引用
- 每個 program 可指定部署到哪些 PE/cluster

建議語法：
```text
pe_program conv_k3 {
	format = "pe-asm-v1"
	deploy = { group: g0, pe_range: "0..47" }
	source_inline = <<'ASM'
		LI r1, 0x100
		...
		HALT
	ASM
}
```

### F) `scan_chain`
- 定義 scan chain words（通常 reverse shift）
- 欄位：`order = reverse|forward`, `words = [0x..., ...]`

### G) `hddu_profiles`
- 每個 profile 對應一組 AGU + HDDU global 配置
- 欄位：
	- `plane_en`, `plane_mode`, `max_outstanding`
	- `agu[PS|PD|PLI|PLO]` 內含 `base/iter/stride/tag/mask/ctrl.ultra`

### H) `dma_plan`
- 定義 DMA descriptors（搬入/搬出）
- 欄位：`name`, `channel`, `src`, `dst`, `bytes`, `burst`, `when`

### I) `temporal_waves`
- 核心：描述 wave 時序與資料重疊
- 每個 wave 包含：
	- `id`
	- `clusters`（group 或 mask）
	- `use_hddu_profile`
	- `dma_prefetch`
	- `dma_drain`
	- `sync_policy`（wait cluster / wait dma / both）

### J) `core_flow`
- 高階控制流程（可選，若未提供則由 compiler 依 wave 自動生成）
- 支援 `for wave in temporal_waves` 類語法

## 2.3 參數撰寫建議

1. 所有地址用 `0x...` 明確標註。
2. `iter` 與 `stride` 全部寫完整 4 維（即使後兩維為 0/1）。
3. `tag` 規則明示 `tag_ctrl`，避免與 `AGU.md` 版本漂移。
4. `cluster_count` 與 `deploy_mask` 必填，避免預設值誤用。
5. 每個 wave 指明「輸入/輸出 buffer slot」以支持 ping-pong。

---

## 3. readable text file 的編譯與轉換流程

## 3.1 編譯管線

```text
.hacc text
	-> (Parser) AST
	-> (Semantic Check) typed IR
	-> (Lowering)
			 - PE asm -> pe_program.bin
			 - core flow -> core_program.bin
			 - scan_chain -> scan_chain.bin
			 - mmio script -> cluster_cfg.bin
			 - dma desc -> dma_desc.bin
	-> (Packager)
			 - loadable package (HAP/ELF)
```

## 3.2 Semantic Check 必做項目

1. `deploy_mask` 不可超過 `cluster_count`
2. `grouping` 不可有重疊 cluster
3. `hddu_profile` 的 AGU base/stride 不可越界 SPM
4. `scan_chain` 字數需符合 `noc_ports * pes_per_port` 需求
5. 每個 wave 的 DMA 搬運量需覆蓋該 wave tensor 需求
6. PE program 部署的 PE 範圍不可超出 `noc_ports * pes_per_port`

## 3.3 建議 IR Struct（compiler 中間格式）

```cpp
struct DeploymentInfo {
	uint32_t cluster_count;
	uint32_t deploy_mask;
	uint32_t cluster_stride;
};

struct AguCfg {
	uint32_t base_addr;
	uint16_t iter[4];
	int32_t  stride[4];
	uint32_t tag_base;
	uint32_t tag_stride0;
	uint32_t tag_stride1;
	uint32_t tag_ctrl;
	uint32_t mask_cfg;
	bool ultra;
};

struct HdduProfile {
	std::string name;
	uint32_t plane_en;
	uint32_t plane_mode;
	uint32_t max_outstanding;
	AguCfg ps, pd, pli, plo;
};

struct WavePlan {
	uint32_t wave_id;
	uint32_t cluster_mask;
	std::string hddu_profile;
	std::vector<std::string> dma_prefetch;
	std::vector<std::string> dma_drain;
	std::string sync_policy;
};
```

## 3.4 輸出 Section 格式建議（可直接對應 Format A/HAP）

### A) Loadable Header（固定）

```cpp
struct CorePkgHeader {
	uint32_t magic;          // 'HACP'
	uint16_t version_major;
	uint16_t version_minor;
	uint32_t section_count;
	uint32_t cluster_count;
	uint32_t deploy_mask;
	uint32_t entry_section;  // 通常指向 CORE_PROGRAM
	uint32_t flags;          // bit0: little-endian
};
```

### B) Section Table

```cpp
enum SectionType : uint32_t {
	SEC_META = 0,
	SEC_CORE_PROGRAM = 1,
	SEC_PE_PROGRAM = 2,
	SEC_SCAN_CHAIN = 3,
	SEC_CLUSTER_CFG = 4,
	SEC_DMA_DESC = 5,
	SEC_WAVE_TABLE = 6,
	SEC_SYMBOL = 7
};

struct SectionDesc {
	uint32_t type;
	uint32_t offset;
	uint32_t size;
	uint32_t cluster_mask;   // 該 section 套用對象
	uint32_t aux;            // type-specific，例如 pe program id
};
```

### C) 各 Section Payload 建議

1. `SEC_CORE_PROGRAM`：core ISA binary words
2. `SEC_PE_PROGRAM`：
	 - header：`program_id`, `im_size_bytes`, `target_pe_bitmap_offset`
	 - body：16-bit instruction stream
3. `SEC_SCAN_CHAIN`：32-bit words（可標記 reverse）
4. `SEC_CLUSTER_CFG`：線性 MMIO script

```cpp
struct MmioOp {
	uint16_t op;      // 0=write,1=poll_eq,2=poll_mask,3=delay
	uint16_t rsvd;
	uint32_t addr;
	uint32_t value;
	uint32_t mask;
	uint32_t timeout;
};
```

5. `SEC_DMA_DESC`：DMA descriptor list
6. `SEC_WAVE_TABLE`：每 wave 對應使用哪些 descriptor/profile/program

## 3.5 Runtime Loader 執行順序建議

1. 讀 header + section table
2. 驗證 `cluster_count` 與目標平台可用 cluster
3. 先套用 `SEC_CLUSTER_CFG` 的靜態設定（SPM map / common MMIO）
4. 載入 `SEC_PE_PROGRAM` + `SEC_SCAN_CHAIN`
5. 啟動 `SEC_CORE_PROGRAM`
6. core program 依 `SEC_WAVE_TABLE` 控制 DMA/cluster

---

## 4. readable text file 範例

> 下述範例重點在格式與資訊完整性，可直接作為 parser/assembler 單元測試輸入。

## 4.1 Conv2D 範例（`conv2d_k3c4.hacc`）

```text
program conv2d_k3c4 {
	meta {
		version = "1.0"
		target = "simulator"
		cluster_count = 2
		deploy_mask = 0b0011
	}

	hardware {
		cluster_stride = 0x00010000
		cluster_mmio_base = 0x00000000
		dma_base = 0x80000000
		noc_ports = 3
		pes_per_port = 16
		spm_size = 0x00100000
		spm_word_bytes = 8
	}

	grouping {
		group g_conv clusters = [0,1]
	}

	tensors {
		tensor W  dtype=i8 shape=[64,3,3,4] layout=O,KH,KW,I spm_base=0x00002000 size_bytes=0x00009000 dma_src="dram://weights.bin"
		tensor X0 dtype=i8 shape=[1,18,202,4] layout=N,H,W,C spm_base=0x00020000 size_bytes=0x00024000 dma_src="dram://ifmap_tile0.bin"
		tensor X1 dtype=i8 shape=[1,18,202,4] layout=N,H,W,C spm_base=0x00044000 size_bytes=0x00024000 dma_src="dram://ifmap_tile1.bin"
		tensor Y0 dtype=i32 shape=[1,16,200,64] layout=N,H,W,C spm_base=0x00070000 size_bytes=0x00080000 dma_src="dram://ofmap_tile0.bin"
		tensor Y1 dtype=i32 shape=[1,16,200,64] layout=N,H,W,C spm_base=0x000F0000 size_bytes=0x00080000 dma_src="dram://ofmap_tile1.bin"
	}

	pe_programs {
		pe_program conv_main {
			format = "pe-asm-v1"
			deploy = { group: g_conv, pe_range: "0..47" }
			source_inline = <<'ASM'
				; PE side pseudo example
				LI r1, 0x0
				; ... vmulu/vaddu/ldma/sdma sequence ...
				HALT
			ASM
		}
	}

	scan_chain {
		order = reverse
		words = [
			0x40100208, 0x40100208, 0x40100208, 0x40100208,
			0x40100208, 0x40100208, 0x40100208, 0x40100208
		]
	}

	hddu_profiles {
		profile conv_wave {
			plane_en = 0xF
			plane_mode = 0x1
			max_outstanding = 16

			agu PS  { base=0x00002000 iter=[4,3,3,1] stride=[1,4,12,36] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
			agu PD  { base=0x00020000 iter=[4,18,1,1] stride=[1,4,72,0] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
			agu PLI { base=0x00070000 iter=[16,1,1,1] stride=[64,0,0,0] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
			agu PLO { base=0x00070000 iter=[16,1,1,1] stride=[64,0,0,0] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
		}
	}

	dma_plan {
		desc load_w  { ch=0 src="dram://weights.bin"      dst=0x00002000 bytes=0x00009000 burst=64 when="init" }
		desc load_x0 { ch=1 src="dram://ifmap_tile0.bin"  dst=0x00020000 bytes=0x00024000 burst=64 when="wave0_prefetch" }
		desc load_x1 { ch=1 src="dram://ifmap_tile1.bin"  dst=0x00044000 bytes=0x00024000 burst=64 when="wave1_prefetch" }
		desc store_y0{ ch=2 src=0x00070000 dst="dram://ofmap_tile0.bin" bytes=0x00080000 burst=64 when="wave0_drain" }
		desc store_y1{ ch=2 src=0x000F0000 dst="dram://ofmap_tile1.bin" bytes=0x00080000 burst=64 when="wave1_drain" }
	}

	temporal_waves {
		wave 0 {
			clusters = g_conv
			use_hddu_profile = conv_wave
			pe_program = conv_main
			dma_prefetch = [load_w, load_x0]
			dma_drain = [store_y0]
			buffer_slot = 0
			sync_policy = "wait_cluster_then_drain"
		}
		wave 1 {
			clusters = g_conv
			use_hddu_profile = conv_wave
			pe_program = conv_main
			dma_prefetch = [load_x1]
			dma_drain = [store_y1]
			buffer_slot = 1
			sync_policy = "overlap_prefetch_with_compute"
		}
	}

	core_flow {
		step init {
			cluster_cfg_map group=g_conv value=0xE4
			load_scan_chain group=g_conv
			load_pe_program group=g_conv program=conv_main
		}
		foreach wave in temporal_waves {
			dma_start wave.dma_prefetch
			hddu_apply group=wave.clusters profile=wave.use_hddu_profile
			pe_start group=wave.clusters
			hddu_start_all group=wave.clusters
			wait_cluster_done group=wave.clusters timeout=1000000
			dma_start wave.dma_drain
			wait_dma_done channels=[1,2] timeout=1000000
		}
	}
}
```

## 4.2 GEMM 範例（`gemm_m128_n128_k64.hacc`）

```text
program gemm_m128_n128_k64 {
	meta {
		version = "1.0"
		target = "simulator"
		cluster_count = 4
		deploy_mask = 0b1111
	}

	hardware {
		cluster_stride = 0x00010000
		cluster_mmio_base = 0x00000000
		dma_base = 0x80000000
		noc_ports = 3
		pes_per_port = 16
		spm_size = 0x00100000
		spm_word_bytes = 8
	}

	grouping {
		group g_all clusters = [0,1,2,3]
		group g_even clusters = [0,2]
		group g_odd  clusters = [1,3]
	}

	tensors {
		tensor A0 dtype=i8  shape=[128,64]  layout=MK spm_base=0x00010000 size_bytes=0x00002000 dma_src="dram://A_tile0.bin"
		tensor A1 dtype=i8  shape=[128,64]  layout=MK spm_base=0x00014000 size_bytes=0x00002000 dma_src="dram://A_tile1.bin"
		tensor B0 dtype=i8  shape=[64,128]  layout=KN spm_base=0x00020000 size_bytes=0x00002000 dma_src="dram://B_tile0.bin"
		tensor B1 dtype=i8  shape=[64,128]  layout=KN spm_base=0x00024000 size_bytes=0x00002000 dma_src="dram://B_tile1.bin"
		tensor C0 dtype=i32 shape=[128,128] layout=MN spm_base=0x00040000 size_bytes=0x00010000 dma_src="dram://C_tile0.bin"
		tensor C1 dtype=i32 shape=[128,128] layout=MN spm_base=0x00060000 size_bytes=0x00010000 dma_src="dram://C_tile1.bin"
	}

	pe_programs {
		pe_program gemm_main {
			format = "pe-asm-v1"
			deploy = { group: g_all, pe_range: "0..47" }
			source_inline = <<'ASM'
				; PE side pseudo example
				; load A/B fragment, MAC, accumulate, store
				HALT
			ASM
		}
	}

	scan_chain {
		order = reverse
		words = [
			0x43188208, 0x43188208, 0x43188208, 0x43188208,
			0x43188208, 0x43188208, 0x43188208, 0x43188208
		]
	}

	hddu_profiles {
		profile gemm_wave {
			plane_en = 0xB   ; PS/PD/PLO
			plane_mode = 0x2 ; gemm
			max_outstanding = 16

			agu PS  { base=0x00010000 iter=[16,8,1,1] stride=[1,16,0,0] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
			agu PD  { base=0x00020000 iter=[16,8,1,1] stride=[1,16,0,0] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
			agu PLI { base=0x00000000 iter=[1,1,1,1]  stride=[0,0,0,0]  tag_base=0 tag_stride0=0 tag_stride1=0 tag_ctrl=0x0 mask=0x0 ultra=false }
			agu PLO { base=0x00040000 iter=[16,8,1,1] stride=[1,16,0,0] tag_base=0 tag_stride0=1 tag_stride1=0 tag_ctrl=0x0 mask=0xF ultra=false }
		}
	}

	dma_plan {
		desc load_a0 { ch=0 src="dram://A_tile0.bin" dst=0x00010000 bytes=0x00002000 burst=64 when="wave0_prefetch" }
		desc load_b0 { ch=1 src="dram://B_tile0.bin" dst=0x00020000 bytes=0x00002000 burst=64 when="wave0_prefetch" }
		desc load_a1 { ch=0 src="dram://A_tile1.bin" dst=0x00014000 bytes=0x00002000 burst=64 when="wave1_prefetch" }
		desc load_b1 { ch=1 src="dram://B_tile1.bin" dst=0x00024000 bytes=0x00002000 burst=64 when="wave1_prefetch" }
		desc store_c0{ ch=2 src=0x00040000 dst="dram://C_tile0.bin" bytes=0x00010000 burst=64 when="wave0_drain" }
		desc store_c1{ ch=2 src=0x00060000 dst="dram://C_tile1.bin" bytes=0x00010000 burst=64 when="wave1_drain" }
	}

	temporal_waves {
		wave 0 {
			clusters = g_all
			use_hddu_profile = gemm_wave
			pe_program = gemm_main
			dma_prefetch = [load_a0, load_b0]
			dma_drain = [store_c0]
			buffer_slot = 0
			sync_policy = "wait_both"
		}
		wave 1 {
			clusters = g_all
			use_hddu_profile = gemm_wave
			pe_program = gemm_main
			dma_prefetch = [load_a1, load_b1]
			dma_drain = [store_c1]
			buffer_slot = 1
			sync_policy = "overlap_prefetch_with_compute"
		}
	}

	core_flow {
		step init {
			cluster_cfg_map group=g_all value=0xE4
			load_scan_chain group=g_all
			load_pe_program group=g_all program=gemm_main
		}
		foreach wave in temporal_waves {
			dma_start wave.dma_prefetch
			hddu_apply group=wave.clusters profile=wave.use_hddu_profile
			pe_start group=wave.clusters
			hddu_start_all group=wave.clusters
			wait_cluster_done group=wave.clusters timeout=1000000
			dma_start wave.dma_drain
			wait_dma_done channels=[2] timeout=1000000
		}
	}
}
```

---

## 5. 實作建議（第一版落地）

1. 先做 `haccc parse`：只產 AST + semantic check。
2. 再做 `haccc build --format hap`：輸出 `*.pkg.hap` + `*.map.json`。
3. Runtime 先吃 `SEC_CLUSTER_CFG + SEC_PE_PROGRAM + SEC_SCAN_CHAIN`，最後再接 `SEC_CORE_PROGRAM`。
4. 先固定 `cluster_count<=8`、`deploy_mask` 32-bit，後續再擴展。
5. wave pipeline 先支援「compute 與下一波 prefetch 重疊」這一種策略即可。

---

## 6. 與現有文件對齊關係

- AGU/HDDU register 欄位：對齊 `doc/AGU.md`、`doc/HDDU.md`。
- Cluster MMIO window：對齊 `doc/ComputeCluster.md`（`0x0000/0x1000/0x2000`）。
- SPM 行為與 mapping：對齊 `doc/SPM.md`。

本文件新增的是「Core Controller 的控制語言與編譯包裝流程」，不改動既有硬體寄存器語意。

---

## 7. 兩個範例經 core-compiler 後的預期 Core ASM（完整 Core Flow）

> 說明：以下為 **core-compiler 預期輸出** 的 `core asm` 文字型態（可作為 `SEC_CORE_PROGRAM` 反組譯視圖）。
> 指令採本文件定義的 core-ISA + pseudo 指令。
> `DMA.START ch, desc_id` 假設 desc_id 由 `SEC_DMA_DESC` 索引。
> `PE.LOAD.SECTION` / `NOC.SCAN.SECTION` 為 compiler 展開用 pseudo（實作時可展成多筆 `CL.MMIO.WI`）。

### 7.1 Conv2D（對應 `conv2d_k3c4.hacc`）

```asm
;============================================================
; core_program.conv2d_k3c4.s
; cluster_count = 2, deploy_mask = 0b0011
; desc_id map:
;   0=load_w, 1=load_x0, 2=load_x1, 3=store_y0, 4=store_y1
;============================================================

.section .text
.global _start

; ---------- constants ----------
.equ M_G_CONV,            0x00000003
.equ CL_MMIO_SPM_MAP,     0x0000
.equ CL_MMIO_SPM_UPDATE,  0x0004
.equ CL_MMIO_NOC_CMD,     0x2000

.equ HDDU_BASE,           0x1000
.equ H_G_CTRL,            0x1800
.equ H_G_STATUS,          0x1804
.equ H_G_PLANE_EN,        0x1808
.equ H_G_PLANE_MODE,      0x180C
.equ H_G_MAX_OUT,         0x1818

; AGU bank base
.equ AGU_PS,              0x1000
.equ AGU_PD,              0x1100
.equ AGU_PLI,             0x1200
.equ AGU_PLO,             0x1300

; AGU register offsets
.equ REG_BASE_ADDR,       0x00
.equ REG_BASE_ADDR_H,     0x04
.equ REG_ITER01,          0x08
.equ REG_ITER23,          0x0C
.equ REG_STRIDE0,         0x10
.equ REG_STRIDE1,         0x14
.equ REG_STRIDE2,         0x18
.equ REG_STRIDE3,         0x1C
.equ REG_CTRL,            0x20
.equ REG_TAG_BASE,        0x40
.equ REG_TAG_STRIDE0,     0x44
.equ REG_TAG_STRIDE1,     0x48
.equ REG_TAG_CTRL,        0x4C
.equ REG_MASK_CFG,        0x54

; packed NoC command immediate (示意)
.equ CMD_RESET,           0x00000000
.equ CMD_INIT_1234,       0x12340001
.equ CMD_START_PE,        0x00000004

; ---------- entry ----------
_start:
	; m0 <- g_conv clusters [0,1]
	CLMASK.SET m0, M_G_CONV

	;--------------------------------------------------------
	; INIT STEP
	;--------------------------------------------------------
init_step:
	; SPM mapping: 0xE4 + update pulse
	CL.MMIO.WI m0, CL_MMIO_SPM_MAP,    0x000000E4
	CL.MMIO.WI m0, CL_MMIO_SPM_UPDATE, 0x00000001

	; NoC init sequence
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_RESET
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_INIT_1234

	; Scan chain（由 section 展開成多筆 mmio）
	NOC.SCAN.SECTION m0, scan_chain_conv_main

	; Load PE program（由 section 展開成 PE.LOAD 序列）
	PE.LOAD.SECTION m0, pe_program_conv_main

	;--------------------------------------------------------
	; WAVE 0
	; dma_prefetch = [load_w, load_x0]
	; dma_drain    = [store_y0]
	; sync_policy  = wait_cluster_then_drain
	;--------------------------------------------------------
wave0_prefetch:
	DMA.START 0, 0      ; load_w
	DMA.START 1, 1      ; load_x0
	DMA.WAIT  0
	DMA.WAIT  1

wave0_apply_hddu_profile:
	; ---- profile conv_wave ----
	; global
	CL.MMIO.WI m0, H_G_CTRL,     0x00000002   ; clear fifo/error
	CL.MMIO.WI m0, H_G_PLANE_EN, 0x0000000F
	CL.MMIO.WI m0, H_G_PLANE_MODE, 0x00000001
	CL.MMIO.WI m0, H_G_MAX_OUT,  0x00000010

	; AGU PS
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR,   0x00002000
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_ITER01,      0x00030004
	CL.MMIO.WI m0, AGU_PS  + REG_ITER23,      0x00010003
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE1,     0x00000004
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE2,     0x0000000C
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE3,     0x00000024
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PS  + REG_CTRL,        0x00000000

	; AGU PD
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR,   0x00020000
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_ITER01,      0x00120004
	CL.MMIO.WI m0, AGU_PD  + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE1,     0x00000004
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE2,     0x00000048
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PD  + REG_CTRL,        0x00000000

	; AGU PLI
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR,   0x00070000
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_ITER01,      0x00010010
	CL.MMIO.WI m0, AGU_PLI + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE0,     0x00000040
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE1,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PLI + REG_CTRL,        0x00000000

	; AGU PLO
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR,   0x00070000
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_ITER01,      0x00010010
	CL.MMIO.WI m0, AGU_PLO + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE0,     0x00000040
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE1,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PLO + REG_CTRL,        0x00000000

wave0_run:
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_START_PE
	CL.MMIO.WI m0, H_G_CTRL, 0x00000004    ; start_all

wave0_wait_cluster:
	; 等待 done 或 err
	CL.MMIO.R.ANY r10, m0, H_G_STATUS
	ANDI r11, r10, 0x00000004
	BNE  r11, r0, error_exit
	ANDI r11, r10, 0x00000002
	BEQ  r11, r0, wave0_wait_cluster

wave0_drain:
	DMA.START 2, 3     ; store_y0
	DMA.WAIT  2

	; 依 core_flow：wave1 啟用 compute 與 prefetch 重疊
	JMP wave1_begin

	;--------------------------------------------------------
	; WAVE 1
	; dma_prefetch = [load_x1]
	; dma_drain    = [store_y1]
	; sync_policy  = overlap_prefetch_with_compute
	;--------------------------------------------------------
wave1_begin:
	DMA.START 1, 2      ; load_x1（prefetch）

	; conv_wave profile 相同，這裡仍完整展開（compiler 可選擇做 common-subroutine）
wave1_apply_hddu_profile:
	CL.MMIO.WI m0, H_G_CTRL,       0x00000002
	CL.MMIO.WI m0, H_G_PLANE_EN,   0x0000000F
	CL.MMIO.WI m0, H_G_PLANE_MODE, 0x00000001
	CL.MMIO.WI m0, H_G_MAX_OUT,    0x00000010

	; PS
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR,   0x00002000
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_ITER01,      0x00030004
	CL.MMIO.WI m0, AGU_PS  + REG_ITER23,      0x00010003
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE1,     0x00000004
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE2,     0x0000000C
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE3,     0x00000024
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PS  + REG_CTRL,        0x00000000

	; PD
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR,   0x00020000
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_ITER01,      0x00120004
	CL.MMIO.WI m0, AGU_PD  + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE1,     0x00000004
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE2,     0x00000048
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PD  + REG_CTRL,        0x00000000

	; PLI
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR,   0x00070000
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_ITER01,      0x00010010
	CL.MMIO.WI m0, AGU_PLI + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE0,     0x00000040
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE1,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PLI + REG_CTRL,        0x00000000

	; PLO
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR,   0x00070000
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_ITER01,      0x00010010
	CL.MMIO.WI m0, AGU_PLO + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE0,     0x00000040
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE1,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PLO + REG_CTRL,        0x00000000

wave1_run:
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_START_PE
	CL.MMIO.WI m0, H_G_CTRL, 0x00000004

wave1_wait:
	; 先等 cluster done
	CL.MMIO.R.ANY r10, m0, H_G_STATUS
	ANDI r11, r10, 0x00000004
	BNE  r11, r0, error_exit
	ANDI r11, r10, 0x00000002
	BEQ  r11, r0, wave1_wait

	; 再等 prefetch x1 完成
	DMA.WAIT 1

wave1_drain:
	DMA.START 2, 4      ; store_y1
	DMA.WAIT  2
	HALT

error_exit:
	; 可擴充：讀取 ERR_CODE 並上報
	HALT
```

### 7.2 GEMM（對應 `gemm_m128_n128_k64.hacc`）

```asm
;============================================================
; core_program.gemm_m128_n128_k64.s
; cluster_count = 4, deploy_mask = 0b1111
; desc_id map:
;   0=load_a0, 1=load_b0, 2=load_a1, 3=load_b1, 4=store_c0, 5=store_c1
;============================================================

.section .text
.global _start

; ---------- constants ----------
.equ M_G_ALL,            0x0000000F
.equ CL_MMIO_SPM_MAP,    0x0000
.equ CL_MMIO_SPM_UPDATE, 0x0004
.equ CL_MMIO_NOC_CMD,    0x2000

.equ H_G_CTRL,           0x1800
.equ H_G_STATUS,         0x1804
.equ H_G_PLANE_EN,       0x1808
.equ H_G_PLANE_MODE,     0x180C
.equ H_G_MAX_OUT,        0x1818

.equ AGU_PS,             0x1000
.equ AGU_PD,             0x1100
.equ AGU_PLI,            0x1200
.equ AGU_PLO,            0x1300

.equ REG_BASE_ADDR,      0x00
.equ REG_BASE_ADDR_H,    0x04
.equ REG_ITER01,         0x08
.equ REG_ITER23,         0x0C
.equ REG_STRIDE0,        0x10
.equ REG_STRIDE1,        0x14
.equ REG_STRIDE2,        0x18
.equ REG_STRIDE3,        0x1C
.equ REG_CTRL,           0x20
.equ REG_TAG_BASE,       0x40
.equ REG_TAG_STRIDE0,    0x44
.equ REG_TAG_STRIDE1,    0x48
.equ REG_TAG_CTRL,       0x4C
.equ REG_MASK_CFG,       0x54

.equ CMD_RESET,          0x00000000
.equ CMD_INIT_5678,      0x56780001
.equ CMD_START_PE,       0x00000004

_start:
	CLMASK.SET m0, M_G_ALL

init_step:
	CL.MMIO.WI m0, CL_MMIO_SPM_MAP,    0x000000E4
	CL.MMIO.WI m0, CL_MMIO_SPM_UPDATE, 0x00000001

	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_RESET
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_INIT_5678
	NOC.SCAN.SECTION m0, scan_chain_gemm_main
	PE.LOAD.SECTION m0, pe_program_gemm_main

	;--------------------------------------------------------
	; WAVE 0 (wait_both)
	; prefetch: load_a0 + load_b0
	; drain:    store_c0
	;--------------------------------------------------------
wave0_prefetch:
	DMA.START 0, 0
	DMA.START 1, 1
	DMA.WAIT  0
	DMA.WAIT  1

wave0_apply_hddu_profile:
	CL.MMIO.WI m0, H_G_CTRL,       0x00000002
	CL.MMIO.WI m0, H_G_PLANE_EN,   0x0000000B
	CL.MMIO.WI m0, H_G_PLANE_MODE, 0x00000002
	CL.MMIO.WI m0, H_G_MAX_OUT,    0x00000010

	; PS
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR,   0x00010000
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_ITER01,      0x00080010
	CL.MMIO.WI m0, AGU_PS  + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE1,     0x00000010
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PS  + REG_CTRL,        0x00000000

	; PD
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR,   0x00020000
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_ITER01,      0x00080010
	CL.MMIO.WI m0, AGU_PD  + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE1,     0x00000010
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PD  + REG_CTRL,        0x00000000

	; PLI disabled profile
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR,   0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_ITER01,      0x00010001
	CL.MMIO.WI m0, AGU_PLI + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE0,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE1,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE0, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_MASK_CFG,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_CTRL,        0x00000000

	; PLO
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR,   0x00040000
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_ITER01,      0x00080010
	CL.MMIO.WI m0, AGU_PLO + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE1,     0x00000010
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PLO + REG_CTRL,        0x00000000

wave0_run:
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_START_PE
	CL.MMIO.WI m0, H_G_CTRL, 0x00000004

wave0_wait_cluster:
	CL.MMIO.R.ANY r10, m0, H_G_STATUS
	ANDI r11, r10, 0x00000004
	BNE  r11, r0, error_exit
	ANDI r11, r10, 0x00000002
	BEQ  r11, r0, wave0_wait_cluster

wave0_drain:
	DMA.START 2, 4
	DMA.WAIT  2

	;--------------------------------------------------------
	; WAVE 1 (overlap_prefetch_with_compute)
	; prefetch: load_a1 + load_b1
	; drain:    store_c1
	;--------------------------------------------------------
wave1_prefetch:
	DMA.START 0, 2
	DMA.START 1, 3

wave1_apply_hddu_profile:
	CL.MMIO.WI m0, H_G_CTRL,       0x00000002
	CL.MMIO.WI m0, H_G_PLANE_EN,   0x0000000B
	CL.MMIO.WI m0, H_G_PLANE_MODE, 0x00000002
	CL.MMIO.WI m0, H_G_MAX_OUT,    0x00000010

	; PS -> A1
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR,   0x00014000
	CL.MMIO.WI m0, AGU_PS  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_ITER01,      0x00080010
	CL.MMIO.WI m0, AGU_PS  + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE1,     0x00000010
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PS  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PS  + REG_CTRL,        0x00000000

	; PD -> B1
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR,   0x00024000
	CL.MMIO.WI m0, AGU_PD  + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_ITER01,      0x00080010
	CL.MMIO.WI m0, AGU_PD  + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE1,     0x00000010
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PD  + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PD  + REG_CTRL,        0x00000000

	; PLI fixed disabled
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR,   0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_ITER01,      0x00010001
	CL.MMIO.WI m0, AGU_PLI + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE0,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE1,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE0, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_MASK_CFG,    0x00000000
	CL.MMIO.WI m0, AGU_PLI + REG_CTRL,        0x00000000

	; PLO -> C1
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR,   0x00060000
	CL.MMIO.WI m0, AGU_PLO + REG_BASE_ADDR_H, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_ITER01,      0x00080010
	CL.MMIO.WI m0, AGU_PLO + REG_ITER23,      0x00010001
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE0,     0x00000001
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE1,     0x00000010
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE2,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_STRIDE3,     0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_BASE,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE0, 0x00000001
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_STRIDE1, 0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_TAG_CTRL,    0x00000000
	CL.MMIO.WI m0, AGU_PLO + REG_MASK_CFG,    0x0000000F
	CL.MMIO.WI m0, AGU_PLO + REG_CTRL,        0x00000000

wave1_run:
	CL.MMIO.WI m0, CL_MMIO_NOC_CMD, CMD_START_PE
	CL.MMIO.WI m0, H_G_CTRL, 0x00000004

wave1_wait_both:
	; cluster done?
	CL.MMIO.R.ANY r10, m0, H_G_STATUS
	ANDI r11, r10, 0x00000004
	BNE  r11, r0, error_exit
	ANDI r11, r10, 0x00000002
	BEQ  r11, r0, wave1_wait_both

	; dma prefetch done?
	DMA.WAIT 0
	DMA.WAIT 1

wave1_drain:
	DMA.START 2, 5
	DMA.WAIT  2
	HALT

error_exit:
	HALT
```

### 7.3 補充：`PE.LOAD.SECTION` / `NOC.SCAN.SECTION` 的展開語意

1. `NOC.SCAN.SECTION mX, scan_chain_*`
   - 對 section words 逐筆產生：`CL.MMIO.WI mX, 0x2000, pack_noc_cmd(CMD_NOC_SCAN_CHAIN, word[i])`

2. `PE.LOAD.SECTION mX, pe_program_*`
   - 對 16-bit 指令流逐筆產生：`CL.MMIO.WI mX, 0x2000, pack_load_program(pc_bytes, inst16)`

3. 若要最終 machine-level core asm（不含 pseudo），compiler 可在最後一個 lowering pass 將這兩者完全展開成連續 `CL.MMIO.WI`。

---

## 8. 大量 Wave（16 ~ 幾萬）時如何避免 Core ASM 爆長

以你給的例子：

- problem shape: `[oc, ic, h, w] = [256, 128, 200, 200]`
- tile: `[16, 4, 16, 200]`
- wave 數量：

$$
N_{wave} = \frac{256}{16} \times \frac{128}{4} \times \frac{200}{16} \times \frac{200}{200} = 16 \times 32 \times 12.5 \times 1
$$

若以整數切分（通常用 ceil）：

$$
N_{wave} = 16 \times 32 \times 13 \times 1 = 6656
$$

若你的 `h` 可整除設定為 192/16，則是 6144；你估算 6400 可視為近似級距。重點是 **量級是數千到上萬波**。

### 8.1 問題根因

目前做法是每個 wave 都展開成：
1. 一串 AGU/MMIO 配置
2. DMA prefetch / drain
3. PE start + HDDU start
4. wait/poll

這是 O(wave_count) 的「程式碼大小」成長，會造成：
- `SEC_CORE_PROGRAM` 體積巨大
- 編譯慢、載入慢
- trace/debug 很難

### 8.2 核心優化原則：把「指令碼」改成「迴圈 + 描述表」

將 core asm 從「展開每一波」改成：

1. **固定控制迴圈（small code）**
2. **Wave Descriptor Table（large data）**

也就是：
- code size 近似 O(1)
- data size O(wave_count)

資料成長可以接受，因為 descriptor 比完整 asm 指令密度高很多，且可壓縮。

### 8.3 建議的 Wave Descriptor 結構（取代展開 ASM）

```cpp
struct WaveDesc {
	uint32_t cluster_mask;
	uint16_t hddu_profile_id;
	uint16_t pe_program_id;
	uint16_t dma_prefetch_begin;
	uint16_t dma_prefetch_count;
	uint16_t dma_drain_begin;
	uint16_t dma_drain_count;
	uint8_t  sync_policy;      // 0=wait_cluster_then_drain,1=wait_both,2=overlap
	uint8_t  flags;            // bit0=need_reconfig, bit1=need_scan_chain ...
	uint16_t reserved;
};
```

並新增 section：
- `SEC_WAVE_DESC`
- `SEC_PROFILE_TABLE`
- `SEC_DMA_DESC`

Core asm 只做「讀 descriptor -> 執行一波 -> next」。

### 8.4 Core ASM 迴圈化範例（可直接當 compiler 模板）

```asm
; r20 = wave_idx
; r21 = wave_count
; r22 = &wave_desc_table

LI   r20, 0
LI   r21, WAVE_COUNT
LI   r22, WAVE_DESC_BASE

wave_loop:
	BEQ  r20, r21, finish

	; desc_ptr = base + idx * sizeof(WaveDesc)
	MULI r23, r20, WAVE_DESC_SIZE
	ADD  r24, r22, r23

	; 讀 descriptor（可透過 DMEM 或 MMIO window）
	LD   r0,  [r24 + 0]      ; cluster_mask
	LD   r1,  [r24 + 4]      ; hddu_profile_id + pe_program_id
	LD   r2,  [r24 + 8]      ; prefetch range
	LD   r3,  [r24 + 12]     ; drain range + sync_policy

	; 1) prefetch
	CALL RUN_DMA_PREFETCH_RANGE

	; 2) apply profile（可做 lazy update，見 8.5）
	CALL APPLY_HDDU_PROFILE

	; 3) run cluster
	CALL START_PE_AND_HDDU

	; 4) wait + drain
	CALL SYNC_AND_DRAIN

	ADDI r20, r20, 1
	JMP  wave_loop

finish:
	HALT
```

這樣你的 core asm 幾乎固定在數十到數百行，不隨 wave 線性暴增。

### 8.5 再進一步：Delta MMIO（只寫改變的欄位）

大量 wave 常見只有 base/offset 在變，`iter/stride/tag_ctrl` 不變。

建議做法：
1. compiler 將 profile 拆成
	 - `static part`（iter/stride/tag_ctrl/mask）
	 - `dynamic part`（base_addr、少數 tag_base）
2. 首波寫 full profile，後續 waves 只寫 dynamic 欄位。

可把每波 MMIO 寫入從 ~60 筆降到 ~8-16 筆。

### 8.6 指令層次優化（強烈建議加 ISA）

新增 3 類 macro 指令，避免 compiler 膨脹：

1. `HDDU.APPLY profile_id`
	 - 硬體/微碼自行展開該 profile 的 MMIO 寫入。

2. `DMA.START.RANGE begin, count`
	 - 連續啟動 descriptor range。

3. `WAVE.EXEC desc_ptr`
	 - 封裝 prefetch + run + wait + drain。

若暫時不改 ISA，也可在 assembler 做 pseudo 指令展開，先享受 code size 優勢。

### 8.7 包裝格式優化（Section 壓縮）

對 `SEC_WAVE_DESC` 可做：
- RLE（連續相同 profile）
- delta-encoding（base addr 只存增量）
- block table（每 64 wave 一個 block header）

例如：

```cpp
struct WaveRun {
	uint16_t repeat;          // 連續 repeat 個 wave
	uint16_t profile_id;
	int32_t  ps_base_delta;
	int32_t  pd_base_delta;
	int32_t  plo_base_delta;
};
```

對規律 tile（conv/gemm）可顯著縮小描述資料。

### 8.8 演算法層次優化：Nest-loop 參數化，而非平面 wave 表

你的 case 本質是 4D tile loop：`oc_t, ic_t, h_t, w_t`。

比起列 6400 筆 wave，可存：
- loop bounds
- base formula
- 每軸 stride delta

讓 core 在 runtime 算每波地址：

$$
base = base_0 + oc_t \cdot \Delta_{oc} + ic_t \cdot \Delta_{ic} + h_t \cdot \Delta_h + w_t \cdot \Delta_w
$$

這會把 `SEC_WAVE_DESC` 從 O(Nwave) 近似壓到 O(維度數)。

### 8.9 實務建議（落地順序）

1. **第一步（最划算）**：導入 `wave loop + WaveDesc`（不再展開每波 asm）。
2. **第二步**：加 `delta MMIO`，只更新 AGU base/tag 動態欄位。
3. **第三步**：加入 `RUN/RANGE` 與 `HDDU.APPLY` pseudo 指令。
4. **第四步**：把規律 workload 升級為 `nest-loop descriptor`。

### 8.10 對 6400 wave 的量化效果（經驗值）

假設 naive 每波 120 條 core 指令：
- 原始：`6400 * 120 = 768,000` 指令

改為 loop + desc：
- 固定控制碼：~200~500 指令
- 每波 runtime 開銷仍在，但不占 code size

若再加 delta MMIO：
- 每波 MMIO transaction 可下降 3~6 倍
- 總執行時間通常也同步下降（MMIO 成為瓶頸時尤明顯）

---

### 8.11 結論

對「數千～數萬 wave」的 task，正確方向不是繼續展開 core asm，而是改成：

`小型固定核心程式 + 大型可壓縮 wave 資料表 + profile/delta 更新`。

這樣同時解決：
- code size
- 編譯時間
- 載入時間
- 可維護性

並保留你現在的 cluster/HDDU/MMIO 架構，不需要大改硬體資料路徑。

---

### 8.12 Core 內部硬體架構（對應第 8 章方法）

以下為建議的 Core Controller 硬體分層，目標是支援「小程式 + 大描述表」。

#### (A) Frontend / Control Pipeline

1. **IFU（Instruction Fetch Unit）**
	- 從 `SEC_CORE_PROGRAM` 所在記憶體抓取 32-bit 指令
	- 支援簡單 branch target buffer（可選）

2. **IDU（Instruction Decode Unit）**
	- 解碼 base ISA 與 pseudo/macro opcode
	- 產生 micro-op 給後端 sequencer

3. **EXE（Integer + Branch）**
	- 執行算術、分支、loop counter 遞增

#### (B) Wave Engine（新核心）

1. **Wave Descriptor Loader**
	- 從 `SEC_WAVE_DESC` 載入 `WaveDesc`
	- 寫入 shadow registers：`cur_cluster_mask`, `cur_profile`, `cur_dma_range`, `cur_sync_policy`

2. **Profile Cache / Delta Unit**
	- 保存 `last_profile_id`, `last_dynamic_fields`
	- 比對本波 descriptor，只產生必要 MMIO writes（delta）

3. **MMIO Sequencer**
	- 接受 micro-op（例如 `HDDU.APPLY`）
	- 轉成有順序保障的 MMIO transaction 串
	- 提供 `fence_ack` 回報給 core pipeline

4. **DMA Range Issuer**
	- 將 `begin/count` 轉成 descriptor 讀取 + `DMA.START`
	- 維護 per-channel inflight counter

5. **Sync/Wait Unit**
	- 監控 cluster done/error、dma done/error、timeout
	- 支援 policy：`wait_cluster_then_drain`, `wait_both`, `overlap`

#### (C) Event / Interrupt Subsystem

1. **IRQ Aggregator**
	- 聚合 cluster / DMA / timeout / software interrupt
	- 對應 `sr.irq_pending`

2. **Trap Controller**
	- error 或 timeout 進 trap vector
	- 保存 `pc/lr/sr` 與 fault code

#### (D) Memory & Table Access

1. **Descriptor SRAM / Cache**（建議）
	- 快取 wave descriptors（例如 64-entry）
	- 減少大量 wave 下的外部記憶體讀取壓力

2. **Profile Table ROM/RAM**
	- 儲存 `SEC_PROFILE_TABLE` 的 static 與 dynamic template

### 8.13 建議硬體資料路徑訊號

```text
Core IF/ID/EX
	-> uop_queue
	-> wave_engine
		 -> mmio_seq -> cluster_mmio_bus
		 -> dma_issue -> dma_mmio_bus
		 -> sync_wait <- {cluster_irq, dma_irq, timeout}
```

### 8.14 `CALL` 與 Wave Engine 的硬體互動

為了讓程式可模組化，建議把以下流程做成可呼叫子程序：

- `CALL RUN_DMA_PREFETCH_RANGE`
- `CALL APPLY_HDDU_PROFILE`
- `CALL START_PE_AND_HDDU`
- `CALL SYNC_AND_DRAIN`

硬體面要求：
1. `CALL/RET` 只作用在 core pipeline，不直接打斷 wave engine state。
2. wave engine 提供 `busy` 狀態；`RET` 前若 `busy=1` 可由 ISA 規定為 illegal 或 stall。
3. trap 發生時需保存 `lr`，確保可回到正確子程序。

### 8.15 最小可實作版本（MVP RTL）

若要快速落地，建議 MVP 只做：

1. `CALL/RET + r31(lr)`
2. `WAVE.LOAD / WAVE.EXEC / WAVE.NEXT` 三個 macro opcode（decode 後進 sequencer）
3. `HDDU.APPLY` 先不做 cache（永遠 full write），功能正確優先
4. 第二版再加 delta/profile cache

這樣可先把「6400 wave 不展開」能力做出來，再逐步優化性能。
