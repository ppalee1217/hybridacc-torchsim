# ComputeCluster 規格與驅動手冊 v3

> 適用檔案：`simulator/include/ComputeCluster.hpp`（目前實作）

## 1. 模組定位

`ComputeCluster` 將下列子模組封裝成單一可驅動單元：

- `ScratchpadMemory`（SPM）
- `HybridDataDeliverUnit`（HDDU）
- `NetworkOnChip`（NoC）

對外提供：

1. 64-bit Data Slave（對接 SPM DMA）
2. 32-bit Command/MMIO Slave（SPM config + HDDU MMIO + NoC command）
3. `interrupt_o`（來自 HDDU，受 power gate 控制）

---

## 2. 參數與關鍵限制

Template 參數（節錄）：

```cpp
template <
		unsigned SPM_NUM_NOC_PORT = 4,
		unsigned SPM_BANKS_PER_GROUP = 3,
		unsigned SPM_BANK_WIDTH_BITS = 64,
		unsigned SPM_ADDR_WIDTH = 32,
		unsigned NOC_NUM_PORTS = 3,
		unsigned NOC_PORT_WIDTH_BITS = 64,
		unsigned NOC_NUM_PES_PER_PORT = 16>
SC_MODULE(ComputeCluster)
```

硬體靜態約束：

- `SPM_NUM_NOC_PORT == 4`
- `SPM_BANK_WIDTH_BITS == 64`
- `NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS == SPM_BANKS_PER_GROUP * SPM_BANK_WIDTH_BITS`

表示 HDDU 與 NoC 的資料寬度需與 SPM group aggregate 寬度一致。

---

## 3. Micro Architecture

## 3.1 Block Diagram

```text
Host Data Slave (64b)                             Host Cmd/MMIO (32b)
data_req/data_rsp                                 cmd_req/cmd_rsp
        |                                                   |
        v                                                   v
+--------------------------------------------------------------------------+
|                              ComputeCluster                               |
|                                                                          |
|  +--------------------+         +------------------+                     |
|  |   MMIO Decoder     |-------->|  NoC command     |----> NoC command_*  |
|  | (SPM/HDDU/NoC)     |         |  (0x2000 only)   |                     |
|  +---------+----------+         +------------------+                     |
|            |                                                           +--+-------------------+
|            +-----------------> HDDU mmio_*                             |   NetworkOnChip     |
|            +-----------------> SPM cfg_*                               | (PS/PD/PLI/PLO VR) |
|                                                                      +--+----------+---------+
|                                                                      ^             |           |
|  +--------------------+      SPM noc-side ports (4x)                | VR req/resp |           |
|  |  ScratchpadMemory  |<------------------------------------------->|             |           |
|  |       (SPM)        |                                             |   +---------v--------+  |
|  |  + DMA 64b port    |<---- data slave bridge ---------------------+---|      HDDU        |  |
|  +--------------------+                                                 | (4 AGU + 4 planes)|  |
|                                                                          +---------+--------+  |
|                                                                                    |           |
|                                                           interrupt_o = power_en & hddu_irq     |
|                                                           local_reset_n = reset_n & power_en    |
+--------------------------------------------------------------------------+
```

## 3.2 內部連線重點

1. **SPM DMA path**
	 - 外部 data slave 直接映射到 SPM DMA 介面
2. **HDDU <-> SPM**
	 - HDDU 四個 SPM port 直連 SPM 四個 noc-side port
	 - `spm.noc_mode_i[0..3]` 固定為 `true`（parallel mode）
3. **HDDU <-> NoC（VR 型別介面）**
	 - `hddu.noc_ps_out` -> `noc.noc_ps_in`
	 - `hddu.noc_pd_out` -> `noc.noc_pd_in`
	 - `hddu.noc_pli_out` -> `noc.noc_pli_in`
	 - `hddu.noc_plo_out` -> `noc.noc_plo_in`
	 - `noc.noc_plo_out` -> `hddu.noc_plo_in`

> 注意：PLO request 不再由 ComputeCluster 的 NoC MMIO 產生，而是由 HDDU PLO channel 直接驅動。

---

## 4. Reset / Power 行為

- 內部 reset 訊號：
	- `local_reset_n = reset_n && power_enable_i`
- `power_enable_i = 0` 時：
	- SPM/HDDU/NoC 全部等效 reset
	- `data_req_rdy_o = 0`
	- `cmd_req_rdy_o = 0`
	- `data_done_o = 0`
	- `interrupt_o = 0`

---

## 5. Top-level 介面

## 5.1 控制與中斷

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `clk` | in | 1 | 系統時脈 |
| `reset_n` | in | 1 | 低有效 reset |
| `power_enable_i` | in | 1 | 電源使能 |
| `interrupt_o` | out | 1 | HDDU interrupt（經 power gate） |

## 5.2 Data Slave（64-bit）

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `data_req_vld_i` | in | 1 | 請求 valid |
| `data_req_rdy_o` | out | 1 | ready |
| `data_addr_i` | in | 32 | global addr |
| `data_write_i` | in | 1 | 1=write, 0=read |
| `data_wdata_i` | in | 64 | write data |
| `data_rdata_o` | out | 64 | read data |
| `data_done_o` | out | 1 | done pulse |

## 5.3 Command/MMIO Slave（32-bit）

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `cmd_req_vld_i` | in | 1 | 請求 valid |
| `cmd_req_rdy_o` | out | 1 | ready |
| `cmd_addr_i` | in | 32 | MMIO addr |
| `cmd_write_i` | in | 1 | 1=write |
| `cmd_wdata_i` | in | 32 | write data |
| `cmd_rdata_o` | out | 32 | read data |
| `cmd_done_o` | out | 1 | accepted pulse |

---

## 6. MMIO Map

## 6.1 SPM config window（`0x0000 ~ 0x00FF`）

| Addr | Name | RW | 說明 |
|---:|---|---|---|
| `0x0000` | `SPM_CONFIG_MAP` | R/W | `[7:0]`：4 ports x 2-bit group mapping |
| `0x0004` | `SPM_CONFIG_UPDATE` | W | `bit0=1` 觸發更新 pulse |
| `0x0008` | `SPM_ARB_POLICY` | R/W | `bit0`（保留） |

## 6.2 HDDU window（`0x1000 ~ 0x1FFF`）

直接 passthrough 到 HDDU MMIO：

- `hddu_mmio_addr = cmd_addr - 0x1000`
- 範例：cluster `0x1800` 對應 HDDU `0x800`

## 6.3 NoC command window（`0x2000 ~ 0x20FF`）

| Addr | Name | RW | 說明 |
|---:|---|---|---|
| `0x2000` | `NOC_CMD_DATA` | R/W | Write：送一拍 `command_mode=1` + `command_data=wdata`；Read：回傳 last command |

> 本版本 **沒有** `0x2004 NOC_PLO_REQ`、`0x2008 NOC_STATUS`。PLO request 由 HDDU 內部通道控制。

---

## 7. 命令分流規則

`cmd_addr` 依區間分流：

1. `0x0000~0x00FF` -> SPM config
2. `0x1000~0x1FFF` -> HDDU MMIO
3. `0x2000~0x20FF` -> NoC command
4. 其他位址 -> 讀回 0、寫入忽略

每次 accepted transaction 都會 pulse `cmd_done_o=1` 一個 cycle。

---

## 8. 驅動流程（通用）

1. `power_enable_i=1`，再釋放 `reset_n`
2. 設定 SPM mapping（`0x0000/0x0004`）
3. 透過 data slave preload 權重/輸入資料到 SPM
4. 設定 HDDU AGU 與 global register（`0x1xxx`）
5. 送 NoC command（`0x2000`）
6. 啟動 HDDU
7. 輪詢 HDDU status/counter 或等待 `interrupt_o`
8. 從 data slave 讀回結果

---

## 9. 驅動範例（完整 AGU MMIO + NoC scan-chain + PE program）

## 9.1 MMIO/Data helper 與命令打包

```cpp
void cmd_write(uint32_t addr, uint32_t data);
uint32_t cmd_read(uint32_t addr);
void data_write64(uint32_t addr, uint64_t data);
uint64_t data_read64(uint32_t addr);

static constexpr uint32_t CLUSTER_HDDU_BASE = 0x1000;
static constexpr uint32_t CLUSTER_NOC_CMD   = 0x2000;
static constexpr uint32_t AGU_BANK_STRIDE   = 0x100;

static constexpr uint32_t AGU_PS  = 0;
static constexpr uint32_t AGU_PD  = 1;
static constexpr uint32_t AGU_PLI = 2;
static constexpr uint32_t AGU_PLO = 3;

// AGU bank offsets
static constexpr uint32_t REG_BASE_ADDR   = 0x00;
static constexpr uint32_t REG_BASE_ADDR_H = 0x04;
static constexpr uint32_t REG_ITER01      = 0x08;
static constexpr uint32_t REG_ITER23      = 0x0C;
static constexpr uint32_t REG_STRIDE0     = 0x10;
static constexpr uint32_t REG_STRIDE1     = 0x14;
static constexpr uint32_t REG_STRIDE2     = 0x18;
static constexpr uint32_t REG_STRIDE3     = 0x1C;
static constexpr uint32_t REG_CTRL        = 0x20;
static constexpr uint32_t REG_LANE_CFG    = 0x28;
static constexpr uint32_t REG_TAG_BASE    = 0x40;
static constexpr uint32_t REG_TAG_STRIDE0 = 0x44;
static constexpr uint32_t REG_TAG_STRIDE1 = 0x48;
static constexpr uint32_t REG_TAG_CTRL    = 0x4C;
static constexpr uint32_t REG_MASK_CFG    = 0x54;

// HDDU global offsets
static constexpr uint32_t HDDU_CTRL            = 0x800;
static constexpr uint32_t HDDU_STATUS          = 0x804;
static constexpr uint32_t HDDU_PLANE_EN        = 0x808;
static constexpr uint32_t HDDU_PLANE_MODE      = 0x80C;
static constexpr uint32_t HDDU_MAX_OUTSTANDING = 0x818;

enum message_command_t : uint32_t {
	CMD_RESET = 0,
	CMD_INIT = 1,
	CMD_LOAD_PROGRAM = 2,
	CMD_STOP_PE = 3,
	CMD_START_PE = 4,
	CMD_NOC_SCAN_CHAIN = 8,
};

static constexpr uint32_t PE_ROUTER_IM_ADDR_OFFSET = 4;
static constexpr uint32_t PE_ROUTER_IM_DATA_OFFSET = 16;
static constexpr uint32_t PE_ROUTER_IM_ADDR_MASK   = 0xFFF;
static constexpr uint32_t PE_ROUTER_IM_DATA_MASK   = 0xFFFF;

uint32_t pack_noc_cmd(message_command_t cmd, uint32_t param) {
	return (param & 0xFFFFFFF0u) | (static_cast<uint32_t>(cmd) & 0x0Fu);
}

uint32_t pack_scan_chain(uint8_t ps_id, uint8_t pd_id, uint8_t pli_id, uint8_t plo_id,
						 uint8_t route_mode, bool enable) {
	uint32_t v = 0;
	v |= (static_cast<uint32_t>(ps_id)  & 0x3Fu) << 4;
	v |= (static_cast<uint32_t>(pd_id)  & 0x3Fu) << 10;
	v |= (static_cast<uint32_t>(pli_id) & 0x3Fu) << 16;
	v |= (static_cast<uint32_t>(plo_id) & 0x3Fu) << 22;
	v |= (static_cast<uint32_t>(route_mode) & 0x03u) << 28;
	v |= (enable ? 1u : 0u) << 30;
	return pack_noc_cmd(CMD_NOC_SCAN_CHAIN, v);
}

uint32_t pack_load_program(uint16_t im_addr_bytes, uint16_t inst16) {
	uint32_t p = 0;
	p |= (static_cast<uint32_t>(im_addr_bytes) & PE_ROUTER_IM_ADDR_MASK) << PE_ROUTER_IM_ADDR_OFFSET;
	p |= (static_cast<uint32_t>(inst16)        & PE_ROUTER_IM_DATA_MASK) << PE_ROUTER_IM_DATA_OFFSET;
	return pack_noc_cmd(CMD_LOAD_PROGRAM, p);
}

void noc_cmd_write(uint32_t packed_cmd) {
	cmd_write(CLUSTER_NOC_CMD, packed_cmd);
}

struct AguCfg {
	uint32_t base_addr;
	uint16_t iter0, iter1, iter2, iter3;
	int32_t stride0, stride1, stride2, stride3;
	uint32_t lane_cfg;
	uint32_t tag_base, tag_stride0, tag_stride1, tag_ctrl;
	uint32_t mask_cfg;
	bool ultra;
};

void cfg_agu(uint32_t bank, const AguCfg& c) {
	const uint32_t B = CLUSTER_HDDU_BASE + bank * AGU_BANK_STRIDE;
	cmd_write(B + REG_BASE_ADDR,   c.base_addr);
	cmd_write(B + REG_BASE_ADDR_H, 0);
	cmd_write(B + REG_ITER01,      (static_cast<uint32_t>(c.iter1) << 16) | c.iter0);
	cmd_write(B + REG_ITER23,      (static_cast<uint32_t>(c.iter3) << 16) | c.iter2);
	cmd_write(B + REG_STRIDE0,     static_cast<uint32_t>(c.stride0));
	cmd_write(B + REG_STRIDE1,     static_cast<uint32_t>(c.stride1));
	cmd_write(B + REG_STRIDE2,     static_cast<uint32_t>(c.stride2));
	cmd_write(B + REG_STRIDE3,     static_cast<uint32_t>(c.stride3));
	cmd_write(B + REG_LANE_CFG,    c.lane_cfg);
	cmd_write(B + REG_TAG_BASE,    c.tag_base);
	cmd_write(B + REG_TAG_STRIDE0, c.tag_stride0);
	cmd_write(B + REG_TAG_STRIDE1, c.tag_stride1);
	cmd_write(B + REG_TAG_CTRL,    c.tag_ctrl);
	cmd_write(B + REG_MASK_CFG,    c.mask_cfg);
	cmd_write(B + REG_CTRL,        c.ultra ? (1u << 3) : 0u);
}
```

## 9.2 Conv2D-like 流程（四 AGU 全設定）

```cpp
power_enable_i = 1;
reset_n = 0; wait_cycles(4);
reset_n = 1; wait_cycles(4);

cmd_write(0x0000, 0xE4);
cmd_write(0x0004, 0x1);

for (uint32_t i = 0; i < weight_words; ++i) data_write64(weight_base + i, weight[i]);
for (uint32_t i = 0; i < ifmap_words;  ++i) data_write64(ifmap_base  + i, ifmap[i]);
for (uint32_t i = 0; i < pli_words;    ++i) data_write64(pli_base    + i, pli_init[i]);

AguCfg ps_cfg  = {ps_base,  ps_i0,  ps_i1,  ps_i2,  ps_i3,  ps_s0,  ps_s1,  ps_s2,  ps_s3,  0, ps_tag_base,  ps_t0,  ps_t1,  ps_tctrl,  0xF, false};
AguCfg pd_cfg  = {pd_base,  pd_i0,  pd_i1,  pd_i2,  pd_i3,  pd_s0,  pd_s1,  pd_s2,  pd_s3,  0, pd_tag_base,  pd_t0,  pd_t1,  pd_tctrl,  0xF, false};
AguCfg pli_cfg = {pli_base, pli_i0, pli_i1, pli_i2, pli_i3, pli_s0, pli_s1, pli_s2, pli_s3, 0, pli_tag_base, pli_t0, pli_t1, pli_tctrl, 0xF, false};
AguCfg plo_cfg = {plo_base, plo_i0, plo_i1, plo_i2, plo_i3, plo_s0, plo_s1, plo_s2, plo_s3, 0, plo_tag_base, plo_t0, plo_t1, plo_tctrl, 0xF, false};

cfg_agu(AGU_PS,  ps_cfg);
cfg_agu(AGU_PD,  pd_cfg);
cfg_agu(AGU_PLI, pli_cfg);
cfg_agu(AGU_PLO, plo_cfg);

cmd_write(CLUSTER_HDDU_BASE + HDDU_CTRL,            (1u << 1)); // clear fifo/error
cmd_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN,        0xF);
cmd_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE,      0x1);
cmd_write(CLUSTER_HDDU_BASE + HDDU_MAX_OUTSTANDING, 16);

noc_cmd_write(pack_noc_cmd(CMD_RESET, 0));
noc_cmd_write(pack_noc_cmd(CMD_INIT,  0x12340000));

// 與 test_noc_sim.cpp 一致：reverse 順序 shift scan-chain
for (int i = static_cast<int>(scan_chain_words.size()) - 1; i >= 0; --i) {
	noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, scan_chain_words[i]));
}

for (uint32_t pc = 0; pc < pe_program_words; ++pc) {
	noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), pe_program[pc]));
}

noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
cmd_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 2));

while (true) {
	uint32_t st = cmd_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
	if (st & (1u << 2)) { /* err */ break; }
	if (st & (1u << 1)) { /* done */ break; }
}

for (uint32_t i = 0; i < ofmap_words; ++i) {
	ofmap[i] = data_read64(ofmap_base + i);
}
```

## 9.3 GEMM-like 流程（四 AGU 全設定）

```cpp
// reset/power/SPM mapping 同 Conv2D

for (uint32_t i = 0; i < A_words; ++i) data_write64(A_base + i, A[i]);
for (uint32_t i = 0; i < B_words; ++i) data_write64(B_base + i, B[i]);
for (uint32_t i = 0; i < C_words; ++i) data_write64(C_base + i, 0);

AguCfg ps_cfg  = {A_base, a_i0, a_i1, a_i2, a_i3, a_s0, a_s1, a_s2, a_s3, 0, a_tag_base, a_t0, a_t1, a_tctrl, 0xF, false};
AguCfg pd_cfg  = {B_base, b_i0, b_i1, b_i2, b_i3, b_s0, b_s1, b_s2, b_s3, 0, b_tag_base, b_t0, b_t1, b_tctrl, 0xF, false};
AguCfg pli_cfg = {pli_base_unused, 1,0,0,0, 0,0,0,0, 0, 0,0,0,0, 0x0, false};
AguCfg plo_cfg = {C_base, c_i0, c_i1, c_i2, c_i3, c_s0, c_s1, c_s2, c_s3, 0, c_tag_base, c_t0, c_t1, c_tctrl, 0xF, false};

cfg_agu(AGU_PS,  ps_cfg);
cfg_agu(AGU_PD,  pd_cfg);
cfg_agu(AGU_PLI, pli_cfg);
cfg_agu(AGU_PLO, plo_cfg);

cmd_write(CLUSTER_HDDU_BASE + HDDU_CTRL,            (1u << 1));
cmd_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN,        0xB); // PS/PD/PLO
cmd_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE,      0x2); // gemm
cmd_write(CLUSTER_HDDU_BASE + HDDU_MAX_OUTSTANDING, 16);

noc_cmd_write(pack_noc_cmd(CMD_RESET, 0));
noc_cmd_write(pack_noc_cmd(CMD_INIT,  0x56780000));
for (int i = static_cast<int>(scan_chain_words.size()) - 1; i >= 0; --i) {
	noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, scan_chain_words[i]));
}
for (uint32_t pc = 0; pc < pe_program_words; ++pc) {
	noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), pe_program[pc]));
}

noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
cmd_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 2));

while ((cmd_read(CLUSTER_HDDU_BASE + HDDU_STATUS) & (1u << 1)) == 0) {}
for (uint32_t i = 0; i < C_words; ++i) C[i] = data_read64(C_base + i);
```

---

## 10. 除錯與驗證建議

建議分層驗證順序：

1. Data slave <-> SPM 基本讀寫
2. SPM mapping/更新脈衝行為
3. HDDU MMIO + AGU 設定讀回
4. NoC command（`0x2000`）
5. 全鏈路 conv/gemm

建議監控：

- HDDU：`COUNTER_TX_PKT / TX_BYTE / RX_BYTE / STALL`
- HDDU：`HDDU_STATUS`（done/err/stall）
- Cluster：`cmd_done_o` 與 `data_done_o` 脈衝一致性
