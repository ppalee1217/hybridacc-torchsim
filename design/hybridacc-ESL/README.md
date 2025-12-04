# HybridAcc ESL Simulator

基於 SystemC 的 HybridAcc 電子系統級 (ESL) 模擬器，用於硬體架構的功能驗證和性能分析。

## 目錄結構

```
hybridacc-ESL/
├── simulator/          # 主要模擬器源碼
│   ├── include/       # 標頭檔
│   │   ├── NoC/       # Network-on-Chip 相關模組
│   │   │   ├── MBUS.hpp
│   │   │   └── NoCRouter.hpp
│   │   ├── PE/        # Processing Element 相關模組
│   │   │   ├── DataLoader.hpp
│   │   │   ├── DataMemory.hpp
│   │   │   ├── Decoder.hpp
│   │   │   ├── EXE_A_stage.hpp
│   │   │   ├── EXE_M_stage.hpp
│   │   │   ├── IF_ID_stage.hpp
│   │   │   ├── InstructionMemory.hpp
│   │   │   ├── LoopController.hpp
│   │   │   ├── PErouter.hpp
│   │   │   ├── PsumRegFile.hpp
│   │   │   ├── TransformRegFile.hpp
│   │   │   ├── VADDU.hpp
│   │   │   └── VMULU.hpp
│   │   ├── ComputeCore.hpp
│   │   ├── CoreDM.hpp
│   │   ├── FIFO.hpp
│   │   ├── HybridAcc.hpp
│   │   ├── NetworkOnChip.hpp
│   │   ├── ProcessElement.hpp
│   │   └── utils.hpp
│   └── src/           # 實作源碼
│       ├── main.cpp
│       └── utils.cpp
└── test/              # 測試程式
    ├── test_pe_sim.cpp
    ├── test_pe_unit.cpp
    └── pe_wrapper.*

```

## 架構概述

HybridAcc 是一個 SIMD 風格的加速器架構，主要由以下模組組成：

### 1. Network-on-Chip (NoC)
- **NoCRouter**: NoC 路由器，負責命令廣播和資料路由
- **MBUS**: Memory Bus，連接 NoC Router 和多個 PE

### 2. Processing Element (PE)
每個 PE 包含三級流水線架構：

#### IF/ID Stage (取指/解碼階段)
- **InstructionMemory**: 指令記憶體 (256 x 16-bit)
- **Decoder**: 指令解碼器
- **LoopController**: 迴圈控制器

#### EXE/M Stage (執行/記憶體階段)
- **DataMemory**: 資料記憶體 (512 x 64-bit)
- **DataLoader**: 資料載入器，支援多種存取模式
- **TransformRegFile**: 轉換暫存器檔案 (8 x 64-bit)
- **VMULU**: 向量乘法單元

#### EXE/A Stage (執行/累加階段)
- **PsumRegFile**: 部分和暫存器檔案 (32 x 64-bit)
- **VADDU**: 向量加法單元

#### PE Router
- **PErouter**: PE 內部路由器，管理 4 個資料端口：
  - PS (Port Static): 靜態資料端口
  - PD (Port Dynamic): 動態資料端口
  - PLI (Port Local Input): 本地網路輸入
  - PLO (Port Local Output): 本地網路輸出

### 3. 資料型別
- **fp16_t**: 16-bit 半精度浮點數
- **v_fp16_t**: 4-lane 向量型別 (4 x fp16_t)
- **pe_inst_t**: 16-bit PE 指令型別

## NoC 通訊協定

### NoC Channels
系統定義了 4 個 NoC 通道：
- **NOC_CHANNEL_PS (0)**: Port Static 通道
- **NOC_CHANNEL_PD (1)**: Port Dynamic 通道
- **NOC_CHANNEL_PLI (2)**: Port Local Input 通道
- **NOC_CHANNEL_PLO (3)**: Port Local Output 通道

### NoC 命令類型

系統透過 `message_command_t` 定義了以下命令：

#### PE 控制命令
這些命令透過 NoC Router 廣播到所有連接的 PE：

| 命令 | 值 | 功能 | 說明 |
|------|-----|------|------|
| `CMD_RESET` | 0 | 清除暫存器 | 重置 PE 內部狀態，清空所有暫存器 |
| `CMD_INIT` | 1 | 初始化配置 | 設定 PE ID、路由模式、啟用狀態 |
| `CMD_LOAD_PROGRAM` | 2 | 載入程式 | 將指令載入到指令記憶體 (IM) |
| `CMD_STOP_PE` | 3 | 停止 PE | 停止 PE 的執行 |
| `CMD_START_PE` | 4 | 啟動 PE | 開始執行 PE 程式 |

#### NoC 配置命令

| 命令 | 值 | 功能 | 說明 |
|------|-----|------|------|
| `CMD_NOC_SCAN_CHAIN` | 8 | Scan-chain 操作 | 配置 PE Router 的路由模式和 ID |

### 命令傳輸格式

#### PE 命令模式 (CMD_RESET ~ CMD_START_PE)
- **傳輸位址**: `0x100` (PE command address)
- **傳輸方式**: 廣播到所有連接的 MBUS 端口
- **資料格式**: 32-bit command_data
  ```
  [3:0]   - 命令類型 (message_command_t)
  [31:4]  - 命令參數
  ```

#### NoC Scan-Chain 命令格式
- **命令值**: `0x8` (CMD_NOC_SCAN_CHAIN)
- **資料格式**: 32-bit command_data
  ```
  [3:0]    - 命令類型 (0x8)
  [9:4]    - ps_id (6 bits)    - Port Static ID
  [15:10]  - pd_id (6 bits)    - Port Dynamic ID
  [21:16]  - pli_id (6 bits)   - Port Local Input ID
  [27:22]  - plo_id (6 bits)   - Port Local Output ID
  [29:28]  - route_mode (2 bits) - 路由模式
  [30]     - enable (1 bit)    - 啟用位元
  ```

### PE Router 路由模式

PE Router 支援 4 種路由模式 (`PERouterMode`)：

| 模式 | 值 | 說明 |
|------|-----|------|
| `PLI_FROM_LN_PLO_TO_LN` | 0b00 | PLI 從本地網路，PLO 到本地網路 |
| `PLI_FROM_BUS_PLO_TO_LN` | 0b01 | PLI 從匯流排，PLO 到本地網路 |
| `PLI_FROM_LN_PLO_TO_BUS` | 0b10 | PLI 從本地網路，PLO 到匯流排 |
| `PLI_FROM_BUS_PLO_TO_BUS` | 0b11 | PLI 從匯流排，PLO 到匯流排 |

### NoC 請求/回應協定

#### Request 格式 (`noc_request_t`)
```cpp
struct noc_request_t {
    uint64_t data;  // 64 bits 資料
    uint16_t addr;  // 9 bits 位址
    bool is_w;      // 寫入旗標 (0: 讀取, 1: 寫入)
};
```

#### Response 格式 (`noc_response_t`)
```cpp
struct noc_response_t {
    uint64_t data;              // 64 bits 資料
    NOC_RESPONSE_STATUS status; // 回應狀態
};
```

#### Response 狀態 (`NOC_RESPONSE_STATUS`)
- `NOC_OK (0)`: 操作成功
- `NOC_ERROR (1)`: 操作錯誤
- `NOC_NOP (2)`: 無操作

### Valid-Ready 介面

系統使用 Valid-Ready 協定進行握手：
- **Valid**: 資料有效信號
- **Ready**: 準備接收信號
- 資料傳輸發生在 `Valid && Ready` 時

介面模板：
- `VRDIF<T>`: Valid-Ready Data Input Interface
- `VRDOF<T>`: Valid-Ready Data Output Interface
- `VRDSIG<T>`: Valid-Ready Data Signal

## 建置說明

### 前置需求
- CMake >= 3.10
- SystemC 2.3.x
- C++14 編譯器

### 建置步驟

1. 設定 SystemC 路徑 (已在 CMakeLists.txt 中設定):
   ```bash
   export SYSTEMC_HOME=/home/yoyo/work/MasterResearch/HybridAcc/libs/systemc
   ```

2. 建立 build 目錄並編譯:
   ```bash
   cd simulator
   mkdir -p build && cd build
   cmake ..
   make
   ```

3. 啟用 Debug 訊息 (可選):
   ```bash
   cmake -DENABLE_DEBUG_UTILS=ON ..
   make
   ```

4. 執行模擬器:
   ```bash
   ./bin/HybridAccSimulator
   ```

## 開發狀態

### ✅ 已完成
- [x] 基本 NoC 架構 (NoCRouter, MBUS)
- [x] PE 三級流水線架構
- [x] 指令記憶體和資料記憶體
- [x] 指令解碼器和控制信號生成
- [x] 向量乘法和加法單元 (VMULU, VADDU)
- [x] 資料載入器 (DataLoader) 支援多種模式
- [x] 迴圈控制器 (LoopController)
- [x] PE Router 和 4 種路由模式
- [x] NoC 命令協定和廣播機制
- [x] Valid-Ready 握手協定
- [x] Stall 邏輯和流水線控制
- [x] 基本測試框架

### 🚧 進行中
- [ ] HybridAcc 頂層模組整合
- [ ] ComputeCore 和 CoreDM 實作
- [ ] 完整的系統級測試
- [ ] 性能分析工具

### 📋 待開發
- [ ] 波形追蹤 (VCD) 輸出
- [ ] GUI 整合
- [ ] 更多測試案例 (卷積、矩陣運算)
- [ ] 文檔補充

## 測試

測試程式位於 `test/` 目錄：
- `test_pe_unit.cpp`: PE 單元測試
- `test_pe_sim.cpp`: PE 模擬測試
- `pe_wrapper.*`: PE 包裝器

執行測試：
```bash
cd test/build
cmake ..
make
./test_pe_unit
```

## Debug 模式

系統提供 `DEBUG_UTILS` 巨集用於除錯：

```cpp
#define DEBUG_MSG(msg) std::cout << "[Debug] " << msg << std::endl;
```

編譯時啟用：
```bash
cmake -DENABLE_DEBUG_UTILS=ON ..
```

## 性能監控

ProcessElement 提供性能監控介面：
```cpp
uint64_t get_cycle_count() const;        // 取得週期數
uint64_t get_instruction_count() const;  // 取得指令數
bool is_running() const;                  // PE 是否運行中
bool is_halted() const;                   // PE 是否停止
void print_stage_status() const;          // 印出流水線狀態
```

## 相關文件

- [PE ISA 文件](../hybridacc-pe-isa/doc/ISA.md)
- [Core ISA 文件](../hybridacc-core-isa/core-ISA.md)
- [GUI 說明](../hybridacc-gui/README.md)

## 授權

此專案屬於碩士研究計畫的一部分。
