# ComputeCluster 規格與驅動手冊 v4

> 適用檔案：`simulator/include/ComputeCluster.hpp`（目前實作）

## 1. 模組定位

`ComputeCluster` 將下列子模組封裝成單一可驅動單元：

- `ScratchpadMemory`（SPM）
- `HybridDataDeliverUnit`（HDDU）
- `NetworkOnChip`（NoC）

對外提供：

1. **AXI4-Lite 64-bit Data Slave**（對接 SPM DMA）
2. **AHB-Lite 32-bit Command/MMIO Slave**（SPM config + HDDU MMIO + NoC command）
3. `interrupt_o`（來自 HDDU，受 power gate 控制）

---

## 2. 參數與關鍵限制

Template 參數（完整）：

```cpp
template <
    unsigned SPM_NUM_NOC_CHANNEL        = 4,
    unsigned SPM_NUM_BANKS_PER_GROUP    = 3,
    unsigned SPM_SRAM_BANK_WIDTH_BITS   = 64,
    unsigned SPM_SRAM_BANK_DEPTH_WORDS  = 8192,
    unsigned SPM_SRAM_BANK_LATENCY      = 1,
    unsigned SPM_SRAM_BANK_PIPELINE_DEPTH = 1,
    unsigned SPM_ADDR_WIDTH             = 32,
    unsigned NOC_NUM_PORTS              = 3,
    unsigned NOC_PORT_WIDTH_BITS        = 64,
    unsigned NOC_NUM_PES_PER_PORT       = 16>
SC_MODULE(ComputeCluster)
```

衍生常數：

```cpp
static constexpr unsigned HDDU_DATA_BITS    = NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS; // 192
static constexpr unsigned HDDU_NOC_TAG_BITS = 6;
static constexpr unsigned HDDU_NOC_ADDR_BITS = HDDU_NOC_TAG_BITS + 1;             // 7
```

硬體靜態約束（`static_assert`）：

- `SPM_NUM_NOC_CHANNEL == 4`
- `SPM_SRAM_BANK_WIDTH_BITS == 64`
- `NOC_NUM_PORTS * NOC_PORT_WIDTH_BITS == SPM_NUM_BANKS_PER_GROUP * SPM_SRAM_BANK_WIDTH_BITS`

  → HDDU/NoC payload 寬度必須與 SPM group aggregate 寬度一致。

---

## 3. Micro Architecture

## 3.1 Block Diagram

```text
AXI4-Lite Data Slave (64b)                       AHB-Lite Command Slave (32b)
s_axi_{aw,w,b,ar,r}_*                            h{sel,addr,write,trans,size,ready,wdata}_i
                                                  h{ready,resp,rdata}_o
        |                                                   |
        v                                                   v
+---------------------------------------------------------------------------------+
|                              ComputeCluster                                     |
|                                                                                 |
|  +-------------------------------+       +----------------------------+         |
|  |  seq_ahb_ctrl (SC_CTHREAD)   |------>|  NoC command sideband      |----> noc.command_{mode,data} |
|  |  AHB Address→Data pipeline   |       |  (AHB 0x2000)              |         |
|  |  SPM config r/w (0x0000)     |       +----------------------------+         |
|  |  HDDU MMIO passthrough       |                                              |
|  |         (0x1000)             |-----> hddu.mmio_{addr,write,wdata,rdata}     |
|  |  SPM cfg signals (0x0000)    |-----> spm.{config_map,config_update,arb}     |
|  |  SPM PMU r/o   (0x0010~)    |<----- spm.pmu_{cycle,arb_stall,credit,port}_cnt_o |
|  +-------------------------------+                                              |
|                                                                   +------------+|
|  comb_power_and_wiring (SC_METHOD)                                | NetworkOnChip|
|  - local_reset_n = reset_n && power_enable_i                     | (PS/PD/PLI/PLO VR)|
|  - AXI4-Lite signals gated by power_enable_i                     +-----+------+|
|  - hready_o  = power_enable_i (no wait states)                         |       |
|  - hresp_o   = 0 (OKAY)                                          VR req/resp   |
|  - interrupt_o = power_enable_i && hddu_interrupt                      |       |
|                                                                   +-----v------+|
|  +--------------------+      SPM noc-side ports (4×)             |    HDDU    ||
|  |  ScratchpadMemory  |<---------------------------------------->| (4 AGU +  ||
|  |       (SPM)        |                                           |  4 planes) ||
|  |  + AXI4-Lite DMA   |<---- AXI4-Lite data slave (gated) -------|            ||
|  |  + PMU counters    |                                           +------------+|
|  +--------------------+                                                        |
+---------------------------------------------------------------------------------+
```

## 3.2 內部連線重點

1. **SPM DMA path**
   - 外部 AXI4-Lite data slave 信號經 `comb_power_and_wiring` 的 power gate 後直連 SPM DMA 介面
   - power off 時，`AWREADY/WREADY/ARREADY = 0`，`BRESP/RRESP = DECERR`

2. **HDDU <-> SPM**
   - HDDU 四個 SPM port 直連 SPM 四個 noc-side port（`SPM_NUM_NOC_CHANNEL = 4`）
   - `SPM_ARB_POLICY` 目前為保留欄位（可寫可讀，但不影響 SPM 仲裁）

3. **HDDU <-> NoC（VR 型別介面）**
   - `hddu.noc_ps_out`  → `noc.noc_ps_in`
   - `hddu.noc_pd_out`  → `noc.noc_pd_in`
   - `hddu.noc_pli_out` → `noc.noc_pli_in`
   - `hddu.noc_plo_out` → `noc.noc_plo_in`
   - `noc.noc_plo_out`  → `hddu.noc_plo_in`

4. **SPM PMU**
   - SPM 輸出 `pmu_cycle_cnt_o`、`pmu_arb_stall_cnt_o`、`pmu_credit_stall_cnt_o`（64-bit）
   - 以及 `pmu_port_txn_cnt_o[0..3]`（64-bit × 4 port）
   - 全部接到對應 `sc_signal`，由 `seq_ahb_ctrl` 讀出供 AHB 讀回

> 注意：PLO request 不再由 ComputeCluster 的 NoC MMIO 產生，而是由 HDDU PLO channel 直接驅動。

---

## 4. Reset / Power 行為

內部 reset 訊號：

```
local_reset_n = reset_n && power_enable_i
```

`power_enable_i = 0` 時：

- SPM / HDDU / NoC 全部等效 reset（接收 `local_reset_n = 0`）
- `s_axi_awready_o / wready_o / arready_o = 0`
- `s_axi_bresp_o / rresp_o = 2'b11`（AXI_RESP_DECERR）
- `hready_o = 0`（AHB slave not ready）
- `interrupt_o = 0`

AHB Command slave 不接受交易（`hready_o = power_enable_i`），無 wait states。

---

## 5. Top-level 介面

## 5.1 控制與中斷

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `clk` | in | 1 | 系統時脈 |
| `reset_n` | in | 1 | 低有效同步重置 |
| `power_enable_i` | in | 1 | 電源使能（0 時整體等效 reset） |
| `interrupt_o` | out | 1 | HDDU interrupt（經 power gate） |

## 5.2 AXI4-Lite Data Slave（64-bit，對接 SPM DMA）

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `s_axi_awvalid_i` | in | 1 | AW channel 有效 |
| `s_axi_awready_o` | out | 1 | `= power_on && spm.awready` |
| `s_axi_awaddr_i` | in | `SPM_ADDR_WIDTH` | 寫入位元組位址 |
| `s_axi_wvalid_i` | in | 1 | W channel 有效 |
| `s_axi_wready_o` | out | 1 | `= power_on && spm.wready` |
| `s_axi_wdata_i` | in | 64 | 寫入資料 |
| `s_axi_wstrb_i` | in | 8 | Byte strobe |
| `s_axi_bvalid_o` | out | 1 | B channel 有效 |
| `s_axi_bready_i` | in | 1 | 上游 B ready |
| `s_axi_bresp_o` | out | 2 | 寫回應（power off 時為 DECERR） |
| `s_axi_arvalid_i` | in | 1 | AR channel 有效 |
| `s_axi_arready_o` | out | 1 | `= power_on && spm.arready` |
| `s_axi_araddr_i` | in | `SPM_ADDR_WIDTH` | 讀取位元組位址 |
| `s_axi_rvalid_o` | out | 1 | R channel 有效 |
| `s_axi_rready_i` | in | 1 | 上游 R ready |
| `s_axi_rdata_o` | out | 64 | 讀回資料（power off 時為 0） |
| `s_axi_rresp_o` | out | 2 | 讀回應（power off 時為 DECERR） |

## 5.3 AHB-Lite Command/MMIO Slave（32-bit）

| Port | Dir | Width | 說明 |
|---|---|---:|---|
| `hsel_i` | in | 1 | Slave select |
| `haddr_i` | in | 32 | 位址（Address phase） |
| `hwrite_i` | in | 1 | 1=write |
| `htrans_i` | in | 2 | 傳輸型態（IDLE=0, BUSY=1, NONSEQ=2, SEQ=3） |
| `hsize_i` | in | 3 | 傳輸寬度（保留，未影響行為） |
| `hburst_i` | in | 3 | burst 型態（保留） |
| `hprot_i` | in | 4 | 保護控制（保留） |
| `hready_i` | in | 1 | 前一個 slave ready（pipeline） |
| `hwdata_i` | in | 32 | 寫入資料（Data phase，比 address late 1 cycle） |
| `hready_o` | out | 1 | `= power_enable_i`（無 wait states） |
| `hresp_o` | out | 1 | `= 0`（OKAY） |
| `hrdata_o` | out | 32 | 讀回資料（power off 時為 0） |

### AHB-Lite 時序語義

AHB-Lite 採用 **兩週期 pipeline**：

```
Cycle N  : Address Phase → hsel & htrans[1]=1 時，鎖存 haddr / hwrite / hsize
Cycle N+1: Data Phase → 根據 N 週期鎖存的位址執行實際讀寫；hwdata 在此週期有效
```

- `HTRANS[1] = 1`（NONSEQ 或 SEQ）且 `hsel=1` 且 `hready_i=1` → 接受 address phase
- `hready_o` 固定為 `power_enable_i`，不引入 wait state
- 讀回資料 `hrdata_o` 由 `ahb_rdata_reg` 在 Data Phase 末尾鎖存，次週期輸出

---

## 6. MMIO Map（AHB 位址）

## 6.1 SPM config window（`0x0000 ~ 0x00FF`）

| Addr | Name | RW | 說明 |
|---:|---|---|---|
| `0x0000` | `SPM_CONFIG_MAP` | R/W | `[7:0]`：4 ports × 2-bit group mapping（`config_map_i`） |
| `0x0004` | `SPM_CONFIG_UPDATE` | W | `bit0=1` 觸發 `config_update_i` pulse（read 回傳 0） |
| `0x0008` | `SPM_ARB_POLICY` | R/W | `bit0`：保留，寫入不影響 SPM 仲裁行為 |
| `0x000C` | `SPM_PMU_CTRL` | W | `bit0=1` 觸發 `pmu_rst_i` pulse（read 回傳 0） |
| `0x0010` | `SPM_PMU_CYCLE_CNT_LO` | R | `pmu_cycle_cnt_o[31:0]` |
| `0x0014` | `SPM_PMU_CYCLE_CNT_HI` | R | `pmu_cycle_cnt_o[63:32]` |
| `0x0018` | `SPM_PMU_ARB_STALL_LO` | R | `pmu_arb_stall_cnt_o[31:0]` |
| `0x001C` | `SPM_PMU_ARB_STALL_HI` | R | `pmu_arb_stall_cnt_o[63:32]` |
| `0x0020` | `SPM_PMU_CREDIT_STALL_LO` | R | `pmu_credit_stall_cnt_o[31:0]` |
| `0x0024` | `SPM_PMU_CREDIT_STALL_HI` | R | `pmu_credit_stall_cnt_o[63:32]` |
| `0x0040 + N×8` | `SPM_PMU_PORTn_TXN_LO` | R | `pmu_port_txn_cnt_o[N][31:0]`（N=0..3） |
| `0x0044 + N×8` | `SPM_PMU_PORTn_TXN_HI` | R | `pmu_port_txn_cnt_o[N][63:32]`（N=0..3） |

備註：`0x0040 + N×0x8` 對應 portN 的 LO，`0x0044 + N×0x8` 對應 HI（N=0..3）。

## 6.2 HDDU window（`0x1000 ~ 0x1FFF`）

直接 passthrough 到 HDDU MMIO（`seq_ahb_ctrl` 保持 `hddu_mmio_write = 1` 一個 cycle）：

- `hddu_mmio_addr = haddr - 0x1000`
- 讀取時同樣設定 `hddu_mmio_addr`，從 `hddu_mmio_rdata` 取值
- 範例：cluster `0x1800` 對應 HDDU `0x800`（Global MMIO）

詳細 HDDU MMIO 欄位請參考 [HDDU.md](HDDU.md)。

## 6.3 NoC command window（`0x2000 ~ 0x20FF`）

| Addr | Name | RW | 說明 |
|---:|---|---|---|
| `0x2000` | `NOC_CMD_DATA` | R/W | Write：發送一拍 `command_mode=1` + `command_data=hwdata`，並更新 `noc_last_cmd_reg`；Read：回傳 `noc_last_cmd_reg` |

> 本版本 **沒有** `0x2004 NOC_PLO_REQ`、`0x2008 NOC_STATUS`。PLO request 由 HDDU PLO channel 直接驅動。

---

## 7. 命令分流規則

`haddr`（AHB）依區間分流（`seq_ahb_ctrl` Data Phase 執行）：

1. `0x0000~0x00FF` → SPM config/PMU 暫存器
2. `0x1000~0x1FFF` → HDDU MMIO passthrough
3. `0x2000~0x20FF` → NoC command sideband
4. 其他位址 → 讀回 0、寫入忽略

---

## 8. 驅動流程（通用）

1. `power_enable_i=1`，再釋放 `reset_n`（至少 4 cycles）
2. 透過 AHB-Lite 設定 SPM mapping（`0x0000/0x0004`）
3. 透過 AXI4-Lite data slave preload 權重/輸入資料到 SPM
4. 透過 AHB-Lite 設定 HDDU AGU 與 global register（`0x1xxx`）
5. 透過 AHB-Lite 送 NoC command（`0x2000`）
6. 透過 AHB-Lite 啟動 HDDU（`0x1800 bit1`）
7. 輪詢 HDDU status（`0x1804`）或等待 `interrupt_o`
8. 透過 AXI4-Lite data slave 讀回結果

---

## 8.1 AXI4-Lite Data Slave 注意事項

- SPM DMA write 僅支援 linear mode 位址映射。
- AXI B channel 採標準 valid/ready：`BVALID` 會持續到 `BREADY` handshake 完成。
- 讀取需等待 `RVALID` 後方可取 `RDATA`。

## 8.2 AHB-Lite Command Slave 注意事項

- 標準 AHB-Lite 2-cycle pipeline：**Address Phase（N）→ Data Phase（N+1）**
- `hready_o` 固定為 `power_enable_i`，不插 wait state
- 寫入：`HTRANS=NONSEQ`，`hwrite=1`，下一週期 `hwdata` 有效
- 讀取：`HTRANS=NONSEQ`，`hwrite=0`，下一週期 `hrdata_o` 有效（Data Phase 末尾更新）

---

## 9. 驅動範例（完整 AGU MMIO + NoC scan-chain + PE program）

## 9.1 MMIO/Data helper 與命令打包

```cpp
// AHB-Lite single transfer helper
void ahb_write(uint32_t addr, uint32_t data);
uint32_t ahb_read(uint32_t addr);

// AXI4-Lite 64-bit DMA helper
void axi_write64(uint32_t addr, uint64_t data);
uint64_t axi_read64(uint32_t addr);

static constexpr uint32_t CLUSTER_HDDU_BASE = 0x1000;
static constexpr uint32_t CLUSTER_NOC_CMD   = 0x2000;
static constexpr uint32_t AGU_BANK_STRIDE   = 0x100;

static constexpr uint32_t AGU_PS  = 0;
static constexpr uint32_t AGU_PD  = 1;
static constexpr uint32_t AGU_PLI = 2;
static constexpr uint32_t AGU_PLO = 3;

// AGU bank offsets（相對於 bank base）
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

// HDDU global offsets（相對於 0x1000）
static constexpr uint32_t HDDU_CTRL       = 0x800;
static constexpr uint32_t HDDU_STATUS     = 0x804;
static constexpr uint32_t HDDU_PLANE_EN   = 0x808;
static constexpr uint32_t HDDU_PLANE_MODE = 0x80C;
static constexpr uint32_t HDDU_ARB_POLICY = 0x818; // 保留欄位

enum message_command_t : uint32_t {
	CMD_RESET        = 0,
	CMD_INIT         = 1,
	CMD_LOAD_PROGRAM = 2,
	CMD_STOP_PE      = 3,
	CMD_START_PE     = 4,
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
	v |= (static_cast<uint32_t>(ps_id)     & 0x3Fu) << 4;
	v |= (static_cast<uint32_t>(pd_id)     & 0x3Fu) << 10;
	v |= (static_cast<uint32_t>(pli_id)    & 0x3Fu) << 16;
	v |= (static_cast<uint32_t>(plo_id)    & 0x3Fu) << 22;
	v |= (static_cast<uint32_t>(route_mode)& 0x03u) << 28;
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
	ahb_write(CLUSTER_NOC_CMD, packed_cmd);
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
	ahb_write(B + REG_BASE_ADDR,   c.base_addr);
	ahb_write(B + REG_BASE_ADDR_H, 0);
	ahb_write(B + REG_ITER01,      (static_cast<uint32_t>(c.iter1) << 16) | c.iter0);
	ahb_write(B + REG_ITER23,      (static_cast<uint32_t>(c.iter3) << 16) | c.iter2);
	ahb_write(B + REG_STRIDE0,     static_cast<uint32_t>(c.stride0));
	ahb_write(B + REG_STRIDE1,     static_cast<uint32_t>(c.stride1));
	ahb_write(B + REG_STRIDE2,     static_cast<uint32_t>(c.stride2));
	ahb_write(B + REG_STRIDE3,     static_cast<uint32_t>(c.stride3));
	ahb_write(B + REG_LANE_CFG,    c.lane_cfg);
	ahb_write(B + REG_TAG_BASE,    c.tag_base);
	ahb_write(B + REG_TAG_STRIDE0, c.tag_stride0);
	ahb_write(B + REG_TAG_STRIDE1, c.tag_stride1);
	ahb_write(B + REG_TAG_CTRL,    c.tag_ctrl);
	ahb_write(B + REG_MASK_CFG,    c.mask_cfg);
	ahb_write(B + REG_CTRL,        c.ultra ? (1u << 3) : 0u);
}
```

## 9.2 Conv2D-like 流程（四 AGU 全設定）

```cpp
power_enable_i = 1;
reset_n = 0; wait_cycles(4);
reset_n = 1; wait_cycles(4);

// SPM group mapping（4 port → group 0/1/2/3）
ahb_write(0x0000, 0xE4);
ahb_write(0x0004, 0x1); // trigger config_update pulse

// Preload data via AXI4-Lite DMA
for (uint32_t i = 0; i < weight_words; ++i) axi_write64(weight_base + i * 8, weight[i]);
for (uint32_t i = 0; i < ifmap_words;  ++i) axi_write64(ifmap_base  + i * 8, ifmap[i]);
for (uint32_t i = 0; i < pli_words;    ++i) axi_write64(pli_base    + i * 8, pli_init[i]);

AguCfg ps_cfg  = {ps_base,  ps_i0,  ps_i1,  ps_i2,  ps_i3,  ps_s0,  ps_s1,  ps_s2,  ps_s3,  0, ps_tag_base,  ps_t0,  ps_t1,  ps_tctrl,  0xF, false};
AguCfg pd_cfg  = {pd_base,  pd_i0,  pd_i1,  pd_i2,  pd_i3,  pd_s0,  pd_s1,  pd_s2,  pd_s3,  0, pd_tag_base,  pd_t0,  pd_t1,  pd_tctrl,  0xF, false};
AguCfg pli_cfg = {pli_base, pli_i0, pli_i1, pli_i2, pli_i3, pli_s0, pli_s1, pli_s2, pli_s3, 0, pli_tag_base, pli_t0, pli_t1, pli_tctrl, 0xF, false};
AguCfg plo_cfg = {plo_base, plo_i0, plo_i1, plo_i2, plo_i3, plo_s0, plo_s1, plo_s2, plo_s3, 0, plo_tag_base, plo_t0, plo_t1, plo_tctrl, 0xF, false};

cfg_agu(AGU_PS,  ps_cfg);
cfg_agu(AGU_PD,  pd_cfg);
cfg_agu(AGU_PLI, pli_cfg);
cfg_agu(AGU_PLO, plo_cfg);

ahb_write(CLUSTER_HDDU_BASE + HDDU_CTRL,       (1u << 0)); // soft-reset / clear FIFO
ahb_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN,   0xF);       // PS/PD/PLI/PLO 全開
ahb_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, 0x1);       // conv mode

noc_cmd_write(pack_noc_cmd(CMD_RESET, 0));
noc_cmd_write(pack_noc_cmd(CMD_INIT,  0x12340000));

// Scan-chain（反序 shift）
for (int i = static_cast<int>(scan_chain_words.size()) - 1; i >= 0; --i) {
	noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, scan_chain_words[i]));
}

// Load PE program
for (uint32_t pc = 0; pc < pe_program_words; ++pc) {
	noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), pe_program[pc]));
}

noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
ahb_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 1)); // start_all

while (true) {
	uint32_t st = ahb_read(CLUSTER_HDDU_BASE + HDDU_STATUS);
	if (st & (1u << 4)) { /* err */ break; }
	if (!(st & (1u << 1))) { /* !any_busy → done */ break; }
}

for (uint32_t i = 0; i < ofmap_words; ++i) {
	ofmap[i] = axi_read64(ofmap_base + i * 8);
}
```

## 9.3 GEMM-like 流程（四 AGU 全設定）

```cpp
// reset/power/SPM mapping 同 Conv2D

for (uint32_t i = 0; i < A_words; ++i) axi_write64(A_base + i * 8, A[i]);
for (uint32_t i = 0; i < B_words; ++i) axi_write64(B_base + i * 8, B[i]);
for (uint32_t i = 0; i < C_words; ++i) axi_write64(C_base + i * 8, 0ULL);

AguCfg ps_cfg  = {A_base, a_i0, a_i1, a_i2, a_i3, a_s0, a_s1, a_s2, a_s3, 0, a_tag_base, a_t0, a_t1, a_tctrl, 0xF, false};
AguCfg pd_cfg  = {B_base, b_i0, b_i1, b_i2, b_i3, b_s0, b_s1, b_s2, b_s3, 0, b_tag_base, b_t0, b_t1, b_tctrl, 0xF, false};
AguCfg pli_cfg = {pli_base_unused, 1,0,0,0, 0,0,0,0, 0, 0,0,0,0, 0x0, false};
AguCfg plo_cfg = {C_base, c_i0, c_i1, c_i2, c_i3, c_s0, c_s1, c_s2, c_s3, 0, c_tag_base, c_t0, c_t1, c_tctrl, 0xF, false};

cfg_agu(AGU_PS,  ps_cfg);
cfg_agu(AGU_PD,  pd_cfg);
cfg_agu(AGU_PLI, pli_cfg);
cfg_agu(AGU_PLO, plo_cfg);

ahb_write(CLUSTER_HDDU_BASE + HDDU_CTRL,       (1u << 0));
ahb_write(CLUSTER_HDDU_BASE + HDDU_PLANE_EN,   0xB); // PS/PD/PLO（PLI 停用）
ahb_write(CLUSTER_HDDU_BASE + HDDU_PLANE_MODE, 0x2); // gemm mode

noc_cmd_write(pack_noc_cmd(CMD_RESET, 0));
noc_cmd_write(pack_noc_cmd(CMD_INIT,  0x56780000));

for (int i = static_cast<int>(scan_chain_words.size()) - 1; i >= 0; --i) {
	noc_cmd_write(pack_noc_cmd(CMD_NOC_SCAN_CHAIN, scan_chain_words[i]));
}

for (uint32_t pc = 0; pc < pe_program_words; ++pc) {
	noc_cmd_write(pack_load_program(static_cast<uint16_t>(pc * 2), pe_program[pc]));
}

noc_cmd_write(pack_noc_cmd(CMD_START_PE, 0));
ahb_write(CLUSTER_HDDU_BASE + HDDU_CTRL, (1u << 1)); // start_all

while ((ahb_read(CLUSTER_HDDU_BASE + HDDU_STATUS) & (1u << 1)) != 0) {}

for (uint32_t i = 0; i < C_words; ++i) C[i] = axi_read64(C_base + i * 8);
```

---

## 10. 除錯與驗證建議

建議分層驗證順序：

1. AXI4-Lite data slave ↔ SPM 基本讀寫
2. AHB-Lite SPM mapping/更新脈衝行為（`0x0000/0x0004`）
3. AHB-Lite HDDU MMIO + AGU 設定讀回（`0x1xxx`）
4. AHB-Lite NoC command（`0x2000`）
5. 全鏈路 conv/gemm

建議監控：

- HDDU：`COUNTER_TX_PKT / TX_BYTE / RX_BYTE / STALL`（`0x1828~0x1834`）
- HDDU：`HDDU_STATUS`（`0x1804`）的 any_busy / err / stall bits
- SPM PMU：`0x0010~0x005C` 各計數器
- `interrupt_o` 脈衝（來自 HDDU status.err 或 status.done）
