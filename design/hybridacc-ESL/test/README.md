# HybridAcc ESL Test Framework

文件樹： [../../../doc/index.md](../../../doc/index.md) -> [../README.md](../README.md) -> 本頁。

這個目錄包含了用於測試 HybridAcc ESL 模擬器的測試框架，涵蓋 ProcessElement (PE) 和 NetworkOnChip (NoC) 的單元測試。

## 目錄結構

```
test/
├── CMakeLists.txt                  # 測試專案 CMake 入口
├── README.md                       # 本文件
├── tb_utils.hpp                    # 共用 testbench utility
├── core_tb_utils.hpp               # core test 專用 utility
├── mvp_compiler.hpp                # 最小 workload/compiler helper
├── core_unit_tests/                # core unit test support files
├── firmware/                       # 測試用韌體與素材
├── test_agu_unit.cpp               # AGU 單元測試
├── test_cluster_control_unit.cpp   # cluster control 單元測試
├── test_cluster_sim.cpp            # cluster 模擬
├── test_cluster_sim_advanced.cpp   # 進階 cluster 模擬
├── test_cluster_unit.cpp           # cluster 單元測試
├── test_core_sim.cpp               # core 模擬
├── test_hddu_unit.cpp              # HDDU 單元測試
├── test_noc_sim.cpp                # NoC 模擬
├── test_noc_unit.cpp               # NoC 單元測試
├── test_pe_sim.cpp                 # PE 模擬測試
├── test_pe_unit.cpp                # PE 單元測試
├── test_spm_unit.cpp               # SPM 單元測試
├── test_sram_unit.cpp              # SRAM 單元測試
└── build/                          # 本地 build 輸出
```

## 測試類型

### 1. PE 單元測試 (`test_pe_unit.cpp`)
測試單個 ProcessElement 的功能，包括：
- 基本操作（重置、初始化）
- 程式載入和執行
- 性能監控（IPC、週期計數）
- Local Network 資料流
- Stall 行為
- 錯誤處理

### 2. PE 模擬測試 (`test_pe_sim.cpp`)
測試 PE 執行實際的運算任務：
- Conv3x3 卷積運算
- 資料載入和結果驗證
- 長時間運行測試

### 3. NoC 單元測試 (`test_noc_unit.cpp`) ⭐ 已完成
測試 NetworkOnChip 架構，包括：
- NoC Router 和 MBUS 互動
- 多 PE 配置和控制
- Scan-chain 配置
- NoC 命令廣播（CMD_RESET, CMD_INIT, CMD_LOAD_PROGRAM, CMD_START_PE, CMD_STOP_PE）
- 4 種 PE Router 路由模式
- NoC 資料傳輸（PS, PD, PLI, PLO 通道）
- 多 PE 同步執行

## 編譯和運行

### 前提條件
- SystemC 已安裝在 `../../../libs/systemc`
- CMake 3.16 或更高版本
- C++17 支援的編譯器（GCC/Clang）

### 除錯選項

CMake 提供三個除錯選項，可以單獨或組合啟用：

| 選項 | 描述 | 適用範圍 |
|------|------|---------|
| `ENABLE_DEBUG_UTILS` | 啟用通用除錯訊息 | 所有模組 |
| `ENABLE_DEBUG_PE` | 啟用 PE 模組除錯訊息 | PE 和 Pipeline 相關 |
| `ENABLE_DEBUG_NOC` | 啟用 NoC 模組除錯訊息 | NoC Router 和 MBUS |

### 編譯步驟

```bash
# 在 test 目錄中
mkdir -p build
cd build

# 基本編譯（無除錯訊息）
cmake ..
make

# 啟用單一除錯選項
cmake -DENABLE_DEBUG_UTILS=ON ..
make

# 啟用多個除錯選項
cmake -DENABLE_DEBUG_UTILS=ON -DENABLE_DEBUG_PE=ON -DENABLE_DEBUG_NOC=ON ..
make

# 清除並重新編譯
make clean
cmake ..
make
```

### 運行測試

```bash
# 方法 1: 直接運行可執行文件
./test_pe_unit          # 運行 PE 單元測試
./test_pe_sim           # 運行 PE 模擬測試
./test_noc_unit         # 運行 NoC 單元測試

# 方法 2: 使用 CMake 自定義目標
make run_tests          # 只運行 PE 單元測試
make run_sim            # 只運行 PE 模擬測試
make run_noc_test       # 只運行 NoC 單元測試
make run_all_tests      # 運行所有單元測試（PE + NoC）

# 方法 3: 使用 CTest
ctest                   # 運行所有測試
ctest -V                # 詳細模式運行所有測試
ctest -R PE_Unit        # 只運行 PE 單元測試
ctest -R NoC_Unit       # 只運行 NoC 單元測試
ctest -R Conv3x3        # 只運行 Conv3x3 模擬測試

# 輸出日誌到文件
./test_noc_unit 2>&1 | tee noc_test.log
```

## 當前構建狀態

### 已編譯產物
- ✅ `libpe_simulator.a` - PE 模擬器靜態庫
- ✅ `test_noc_unit` - NoC 單元測試可執行文件

### 待編譯
如果缺少某些可執行文件，請運行：
```bash
cd build
make test_pe_unit       # 編譯 PE 單元測試
make test_pe_sim        # 編譯 PE 模擬測試
make                    # 編譯所有目標
```

## NoC 單元測試詳細說明

### 測試架構
- **配置**: 2 個 Port，每個 Port 有 2 個 PE（總共 4 個 PE）
- **測試方式**: 使用 TestBench 類別封裝測試邏輯
- **時鐘週期**: 10 ns

### 測試案例

#### Test 1: Reset and Initialization
- 測試系統重置功能
- 驗證初始狀態

#### Test 2: Scan-Chain Configuration
- 測試所有 PE 的 Scan-chain 配置
- 配置 PS/PD/PLI/PLO ID 和路由模式

#### Test 3: CMD_RESET Command
- 測試 CMD_RESET 命令廣播
- 驗證所有 PE 接收並執行重置

#### Test 4: CMD_INIT Command
- 測試 CMD_INIT 命令廣播
- 驗證初始化參數傳遞

#### Test 5: Program Loading
- 測試透過 NoC 載入程式到所有 PE
- 驗證程式載入機制

#### Test 6: Start PE Execution
- 測試 CMD_START_PE 命令
- 驗證所有 PE 同步啟動

#### Test 7: Stop PE Execution
- 測試 CMD_STOP_PE 命令
- 驗證所有 PE 停止執行

#### Test 8: Multiple Router Modes
- 測試 4 種 PE Router 路由模式：
  - `PLI_FROM_LN_PLO_TO_LN`
  - `PLI_FROM_BUS_PLO_TO_LN`
  - `PLI_FROM_LN_PLO_TO_BUS`
  - `PLI_FROM_BUS_PLO_TO_BUS`

#### Test 9: Sequential Program Execution
- 完整流程測試：配置 → 載入 → 執行 → 停止
- 驗證多 PE 協同工作

#### Test 10: NoC Data Transfer
- 測試透過不同通道的資料傳輸
- 驗證 PS 和 PD 通道的讀寫

### NoC TestBench API

```cpp
class NoCTestBench {
    // 系統控制
    void reset_system();
    void run_cycles(int n);

    // 命令發送
    void send_command(message_command_t cmd, uint32_t param = 0);
    void send_scan_chain_config(uint8_t ps_id, uint8_t pd_id,
                                uint8_t pli_id, uint8_t plo_id,
                                PERouterMode mode, bool enable);

    // 程式載入
    void load_program_to_all_pes(const std::vector<uint16_t>& program);
};
```

## PE 測試框架

### PE Wrapper Class 功能

PEWrapper 類提供了簡化的介面來測試 ProcessElement：

#### 測試控制
```cpp
pe.reset();                      // 重置 PE
pe.start();                      // 啟動 PE（透過 NoC 發送 START 命令）
pe.stop();                       // 停止 PE（透過 NoC 發送 STOP 命令）
pe.run_cycles(100);              // 運行 100 個時鐘週期
pe.run_until_halt(1000);         // 運行直到 PE 暫停或超時
```

#### 程式載入
```cpp
pe.load_program("program.bin");           // 從檔案載入
pe.load_program(vector<uint16_t>{...});   // 從陣列載入
pe.load_instruction(addr, inst);          // 載入單一指令
```

#### 資料介面
```cpp
pe.send_noc_request(addr, data);   // 透過 NoC 發送請求
pe.read_noc_response(data);        // 讀取 NoC 響應
pe.send_pli_data(data);            // 透過 PLI 發送資料
pe.read_plo_data(data);            // 從 PLO 讀取資料
```

#### 狀態監控
```cpp
pe.is_running();                   // 檢查是否運行中
pe.is_halted();                    // 檢查是否已停止
pe.is_busy();                      // 檢查是否忙碌
pe.get_cycle_count();              // 獲取週期計數
pe.get_instruction_count();        // 獲取指令計數
pe.get_performance_metrics();      // 獲取性能指標
```

#### 除錯工具
```cpp
pe.set_debug(true);                // 啟用除錯模式
pe.dump_state();                   // 輸出 PE 狀態
pe.dump_instruction_memory();      // 輸出指令記憶體
pe.print_stage_status();           // 輸出流水線狀態
pe.save_trace("trace.txt");        // 保存執行軌跡
```

## PE 指令編碼

測試中使用的指令編碼：

```cpp
const uint16_t NOP_INST = 0x0004;   // opcode=2, funct2=0
const uint16_t HALT_INST = 0x001E;  // opcode=3, funct2=3
```

完整的指令集請參考 [PE ISA 文檔](../../hybridacc-pe-isa/doc/ISA.md)。

## NoC 命令協定

### 命令類型

| 命令 | 值 | 功能 |
|------|-----|------|
| `CMD_RESET` | 0 | 清除暫存器 |
| `CMD_INIT` | 1 | 初始化配置 |
| `CMD_LOAD_PROGRAM` | 2 | 載入程式 |
| `CMD_STOP_PE` | 3 | 停止 PE |
| `CMD_START_PE` | 4 | 啟動 PE |
| `CMD_NOC_SCAN_CHAIN` | 8 | Scan-chain 配置 |

### Scan-Chain 資料格式

```
[3:0]    - 命令類型 (0x8)
[9:4]    - ps_id (6 bits)
[15:10]  - pd_id (6 bits)
[21:16]  - pli_id (6 bits)
[27:22]  - plo_id (6 bits)
[29:28]  - route_mode (2 bits)
[30]     - enable (1 bit)
```

## 使用範例

### 測試單個 PE

```cpp
#include "pe_wrapper.hpp"

int sc_main(int argc, char* argv[]) {
    PEWrapper pe("TestPE");
    pe.set_debug(true);
    pe.reset();

    // 載入並執行程式
    std::vector<uint16_t> program = {0x0004, 0x001E};  // NOP + HALT
    pe.load_program(program);
    pe.start();

    bool completed = pe.run_until_halt(1000);
    if (completed) {
        auto metrics = pe.get_performance_metrics();
        std::cout << "IPC: " << metrics.ipc << std::endl;
    }

    return 0;
}
```

### 測試 NoC 多 PE 系統

```cpp
#include "NetworkOnChip.hpp"

int sc_main(int argc, char* argv[]) {
    // 創建 NoC：2 ports, 2 PEs per port
    NetworkOnChip noc("NoC", 2, 2);

    // ... 連接信號和時鐘 ...

    // 發送配置命令
    send_scan_chain_config(0, 1, 2, 3,
                          PERouterMode::PLI_FROM_BUS_PLO_TO_BUS, true);

    // 載入程式並啟動
    load_program_to_all_pes(program);
    send_command(message_command_t::CMD_START_PE);

    // 運行模擬
    sc_start(1000, SC_NS);

    return 0;
}
```

## 故障排除

### 編譯錯誤

1. **找不到 SystemC**
   ```bash
   # 檢查路徑
   ls ../../../libs/systemc/lib-linux64
   # 設置環境變量
   export SYSTEMC_HOME=/path/to/systemc
   ```

2. **找不到 NetworkOnChip.hpp**
   ```bash
   # 確認 simulator 標頭檔存在
   ls ../simulator/include/NetworkOnChip.hpp
   ```

3. **鏈接錯誤：undefined reference**
   ```bash
   # 清除並重新編譯
   cd build
   rm -rf *
   cmake ..
   make
   ```

### 運行時錯誤

1. **NoC 測試超時**
   - 啟用 DEBUG_NOC 查看詳細日誌：
     ```bash
     cd build
     cmake -DENABLE_DEBUG_NOC=ON ..
     make
     ./test_noc_unit
     ```
   - 檢查 PE 是否正確配置
   - 確認命令格式正確

2. **PE 沒有啟動**
   - 確認已發送 CMD_START_PE 命令
   - 檢查 scan-chain 配置是否正確
   - 驗證 router_enable 信號為 true
   - 啟用 DEBUG_PE 查看 PE 內部狀態

3. **SystemC 運行時錯誤**
   ```bash
   # 檢查 SystemC 庫路徑
   ldd ./test_noc_unit
   # 設置 LD_LIBRARY_PATH
   export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../../libs/systemc/lib-linux64
   ```

### 除錯技巧

```bash
# 啟用所有除錯訊息
cd build
cmake -DENABLE_DEBUG_UTILS=ON -DENABLE_DEBUG_PE=ON -DENABLE_DEBUG_NOC=ON ..
make

# 運行測試並保存日誌
./test_noc_unit 2>&1 | tee noc_test.log

# 使用 CTest 的詳細模式
ctest -V -R NoC_Unit

# 使用 gdb 除錯
gdb ./test_noc_unit
(gdb) run
(gdb) backtrace
```

### 常見問題

**Q: 為什麼 `make run_all_tests` 不包含 `test_pe_sim`？**
A: `run_all_tests` 目標只運行單元測試，不包含長時間運行的模擬測試。使用 `make run_sim` 單獨運行。

**Q: 如何查看 PE 的執行軌跡？**
A: 啟用 `ENABLE_DEBUG_PE` 或在程式中使用 `pe.save_trace("trace.txt")`。

**Q: NoC 測試卡住不動？**
A: 可能是時鐘未正確配置或 PE 處於等待狀態。檢查時鐘信號和 PE 的 enable 狀態。

## 擴展測試

### 添加新的 PE 測試

1. 在 `test_pe_unit.cpp` 中添加新的測試函數
2. 使用 PEWrapper API 編寫測試邏輯
3. 重新編譯並運行

```cpp
void test_new_feature() {
    std::cout << "\n=== Test: New Feature ===" << std::endl;
    PEWrapper pe("TestPE");
    pe.reset();

    // 測試邏輯
    // ...

    std::cout << "✓ New feature test passed" << std::endl;
}
```

### 添加新的 NoC 測試

1. 在 `test_noc_unit.cpp` 的 `test_main()` 中添加新測試
2. 使用 NoCTestBench API 編寫測試邏輯
3. 重新編譯並運行

```cpp
// Test 11: New NoC Feature
std::cout << "\n=== Test 11: New NoC Feature ===" << std::endl;
tb.reset_system();
// 測試邏輯
// ...
std::cout << "✓ Test 11 passed" << std::endl;
```

### 添加新的測試文件

1. 創建新的 `.cpp` 測試文件（例如 `test_memory.cpp`）
2. 在 `CMakeLists.txt` 中添加可執行目標：

```cmake
add_executable(test_memory
    test_memory.cpp
)

target_link_libraries(test_memory
    pe_simulator
    systemc
    pthread
)

add_test(NAME Memory_Tests COMMAND test_memory)
```

3. 重新運行 CMake 並編譯

## 性能基準

### PE 單元測試
- 預期執行時間：< 1 秒
- 測試案例：6 個
- 覆蓋率：基本功能 100%

### PE 模擬測試（Conv3x3）
- 預期執行時間：< 5 秒
- 測試類型：實際卷積運算
- 驗證：輸出正確性

### NoC 單元測試
- 預期執行時間：< 2 秒
- 測試案例：10 個
- PE 配置：4 個 PE (2 Port × 2 PE/Port)
- 覆蓋率：NoC 基本功能 100%

## 測試檢查清單

在提交代碼前，確保：

- [ ] 所有測試編譯成功（無警告）
- [ ] `make run_all_tests` 執行成功
- [ ] NoC 測試的所有 10 個案例通過
- [ ] PE 單元測試的所有案例通過
- [ ] Conv3x3 模擬測試輸出正確
- [ ] 無記憶體洩漏（使用 valgrind 檢查）
- [ ] 代碼符合專案風格指南

## 相關文件

- [ESL Simulator README](../README.md)
- [ESL Documentation Index](../doc/index.md)
- [PE ISA 文檔](../../hybridacc-pe-isa/doc/ISA.md)
- [PE 架構文檔](../../hybridacc-pe-isa/doc/HybridaccPE.md)
- [Compiler 文件入口](../../hybridacc-cc/doc/00_Overview.md)

## 貢獻指南

如果要添加新的測試或改進現有測試：

1. 確保新測試獨立且可重複
2. 添加適當的註釋和文檔
3. 更新此 README 文件
4. 確保所有現有測試仍然通過
5. 考慮添加 CTest 測試案例

## 授權

此專案屬於碩士研究計畫的一部分。