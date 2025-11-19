# PE Unit Test Framework

這個目錄包含了用於測試 ProcessElement (PE) 的單元測試框架。

## 文件結構

- `pe_wrapper.hpp` - PE Wrapper Class 頭文件
- `pe_wrapper.cpp` - PE Wrapper Class 實現
- `test_pe_unit.cpp` - 具體的測試案例
- `CMakeLists.txt` - CMake 構建配置

## 架構說明

### PE 控制方式

PE 的控制已從傳統的 `pe_start` 信號改為**通過 NoC (Network-on-Chip) 介面進行命令控制**：

- **CMD_LOAD_PROGRAM (2)**: 載入指令到指令記憶體
- **CMD_STOP_PE (3)**: 停止 PE 執行
- **CMD_START_PE (4)**: 啟動 PE 執行

命令通過 NoC 請求介面發送到 PE 的命令地址 (`PE_CMD_ADDRESS = 0x100`)。

### 介面說明

PE 提供以下主要介面：

1. **NoC 介面** - 用於命令控制和記憶體訪問
   - `noc_req_in` / `noc_req_in_valid` / `noc_req_in_ready` - NoC 請求輸入
   - `noc_resp_out` / `noc_resp_out_valid` / `noc_resp_out_ready` - NoC 響應輸出

2. **Local Network 介面** - 用於資料流傳輸
   - `ln_pli_in_*` - Port Local Input (PLI) 輸入資料
   - `ln_plo_out_*` - Port Local Output (PLO) 輸出資料

3. **控制信號**
   - `reset_n` - 重置信號
   - `pe_busy` - PE 忙碌狀態

## PE Wrapper Class 功能

PEWrapper 類提供了一個簡化的介面來測試 ProcessElement，包含以下主要功能：

### 測試控制
- `reset(int)` - 重置 PE 並初始化所有信號
- `start()` - 通過 NoC 發送 START 命令啟動 PE 執行
- `stop()` - 通過 NoC 發送 STOP 命令停止 PE 執行
- `run_cycles(int)` - 運行指定數量的時鐘週期
- `run_until_halt(int)` - 運行直到 PE 暫停或超時

### 程式載入
- `load_program(filename)` - 從二進制文件載入程式
- `load_program(vector<uint16_t>)` - 從指令陣列載入程式（通過 NoC CMD_LOAD_PROGRAM）
- `load_instruction(addr, inst)` - 載入單一指令到指定地址

### 資料介面
- `send_noc_request(addr, data)` - 通過 NoC 介面發送請求（支援 ready/valid 握手）
- `read_noc_response(data)` - 讀取 NoC 響應資料（支援 ready/valid 握手）
- `send_pli_data(data)` - 通過 Local Network PLI 發送資料
- `read_plo_data(data)` - 讀取 Local Network PLO 資料

### 狀態監控
- `is_running()` - 檢查 PE 是否正在運行
- `is_halted()` - 檢查 PE 是否已暫停
- `is_busy()` - 檢查 PE 是否忙碌
- `get_cycle_count()` - 獲取當前週期計數
- `get_instruction_count()` - 獲取已執行指令數量
- `get_performance_metrics()` - 獲取性能指標（包括 IPC、stall cycles 等）

### 除錯工具
- `set_debug(bool)` - 啟用/禁用除錯日志
- `dump_state()` - 輸出當前 PE 狀態
- `dump_instruction_memory()` - 輸出指令記憶體內容
- `print_stage_status()` - 輸出流水線各階段狀態
- `save_trace(filename)` - 保存執行軌跡到文件

### 斷言輔助
- `assert_completion(max_cycles, message)` - 斷言 PE 在超時內完成
- `assert_instruction_count(expected, message)` - 斷言預期的指令數量
- `assert_performance(min_ipc, message)` - 斷言性能標準

## 測試案例

當前包含的測試案例：

1. **基本操作測試** (`Test 1`)
   - 測試 PE 初始化和重置
   - 驗證初始狀態（not running, not halted, not busy）

2. **簡單程式執行** (`Test 2`)
   - 測試載入和執行基本程式（2 個 NOP + HALT）
   - 驗證程式能正常完成並進入 halt 狀態

3. **性能監控測試** (`Test 3`)
   - 測試性能指標收集（4 個 NOP + HALT）
   - 驗證週期計數、指令計數和 IPC 計算

4. **本地網路資料流測試** (`Test 4`)
   - 測試 PLI/PLO 資料傳輸
   - 驗證 ready/valid 握手機制

5. **Stall 行為測試** (`Test 5`)
   - 測試長時間運行和流水線停頓行為
   - 監控執行過程中的狀態變化

6. **錯誤處理測試** (`Test 6`)
   - 測試斷言和錯誤處理機制
   - 驗證完成性和指令計數斷言

## PE 指令編碼

測試中使用的指令編碼：

- **NOP**: `0x0004` (opcode=2, funct2=0)
- **HALT**: `0x001E` (opcode=3, funct2=3)

指令編碼格式請參考 PE ISA 文檔。

## 編譯和運行

### 前提條件
- SystemC 已安裝在 `../../../libs/systemc`
- CMake 3.16 或更高版本
- C++17 支援的編譯器（GCC/Clang）

### 編譯步驟
```bash
# 在 test 目錄中
mkdir -p build
cd build

# 基本編譯
cmake ..
make

# 或啟用除錯訊息
cmake -DENABLE_DEBUG_UTILS=ON ..
make
```

### 運行測試
```bash
# 運行所有測試
./test_pe_unit

# 或使用 CMake 目標
make run_tests

# 或使用 CTest
ctest
```

### 編譯選項

- `ENABLE_DEBUG_UTILS`: 啟用 DEBUG_UTILS 除錯訊息（預設：OFF）

```bash
cmake -DENABLE_DEBUG_UTILS=ON ..
```

## 使用範例

### 基本使用

```cpp
#include "pe_wrapper.hpp"
using namespace hybridacc::test;

int sc_main(int argc, char* argv[]) {
    // 創建 PE wrapper
    PEWrapper pe("MyTest");
    pe.set_debug(true);

    // 重置 PE
    pe.reset();

    // 載入程式（NOP + HALT）
    std::vector<uint16_t> program = {0x0004, 0x001E};
    pe.load_program(program);

    // 啟動執行（通過 NoC 發送 START 命令）
    pe.start();

    // 運行直到完成
    bool completed = pe.run_until_halt(1000);

    if (completed) {
        pe.dump_state();
        auto metrics = pe.get_performance_metrics();
        std::cout << "IPC: " << metrics.ipc << std::endl;
    }

    return 0;
}
```

### 使用 NoC 介面

```cpp
// 發送自定義命令
uint64_t cmd = 4;  // CMD_START_PE
bool success = pe.send_noc_request(0x100, cmd);

// 讀取響應
uint64_t response_data;
if (pe.read_noc_response(response_data)) {
    std::cout << "Response: 0x" << std::hex << response_data << std::endl;
}
```

### 使用 Local Network 介面

```cpp
// 發送資料到 PLI
uint64_t input_data = 0x123456789ABCDEF0;
bool sent = pe.send_pli_data(input_data);

// 從 PLO 讀取資料
uint64_t output_data;
if (pe.read_plo_data(output_data)) {
    std::cout << "Output: 0x" << std::hex << output_data << std::endl;
}
```

## 擴展測試

要添加新的測試案例：

1. 在 `test_pe_unit.cpp` 中添加新的測試函數
2. 使用正確的 PE 指令編碼（參考 PE ISA 文檔）
3. 在 `sc_main()` 函數中呼叫新的測試函數
4. 重新編譯和運行

### 測試特定 PE 指令的步驟

1. 參考 `design/hybridacc-pe-isa/doc/ISA.md` 了解指令編碼
2. 創建包含目標指令的程式陣列
3. 使用 `load_program()` 通過 NoC 載入程式
4. 使用 PLI 介面提供輸入資料（如需要）
5. 啟動執行並監控狀態
6. 從 PLO 介面讀取輸出結果
7. 檢查性能指標

## 注意事項

### 控制流程
- PE 的啟動和停止現在**完全通過 NoC 命令**控制，不再使用 `pe_start` 信號
- `start()` 方法會自動發送 `CMD_START_PE` 命令並等待 5 個週期讓 PE 處理命令
- 程式載入通過 `CMD_LOAD_PROGRAM` 命令完成，每條指令需要一個週期處理

### 握手協議
- NoC 和 Local Network 介面都使用 **ready/valid 握手協議**
- 發送資料時需等待 ready 信號
- 接收資料時需等待 valid 信號
- 預設超時設定為 100 個週期

### 指令編碼
- 確保使用正確的指令編碼格式
- NOP 和 HALT 指令已在測試中驗證
- 其他指令需參考 PE ISA 文檔

### 性能監控
- IPC (Instructions Per Cycle) 計算基於實際執行的指令數和週期數
- Stall cycle 監控功能為基本實現，可能需要進一步增強
- 性能指標收集依賴於 ProcessElement 類提供的介面

## 故障排除

### 編譯錯誤

1. **找不到 SystemC**
   - 檢查 SystemC 路徑：`../../../libs/systemc`
   - 確認 `lib-linux64` 目錄存在

2. **缺少頭文件**
   - 確認 PE simulator 頭文件在 `../simulator/include`
   - 檢查所有必要的頭文件都已包含

3. **C++17 支援**
   - 確認編譯器版本支援 C++17
   - GCC 7+ 或 Clang 5+

### 運行時錯誤

1. **PE 沒有啟動**
   - 確認已呼叫 `reset()` 初始化
   - 確認已呼叫 `start()` 發送啟動命令
   - 啟用除錯模式查看詳細日志：`pe.set_debug(true)`

2. **超時錯誤**
   - 檢查程式是否包含 HALT 指令
   - 增加 `run_until_halt()` 的超時值
   - 使用 `dump_state()` 檢查 PE 狀態

3. **資料傳輸失敗**
   - 檢查 ready/valid 信號狀態
   - 確認握手協議正確實現
   - 驗證資料介面配置

### 除錯技巧

```cpp
// 啟用除錯模式
pe.set_debug(true);

// 輸出 PE 狀態
pe.dump_state();

// 輸出指令記憶體
pe.dump_instruction_memory();

// 輸出流水線狀態
pe.print_stage_status();

// 保存執行軌跡
pe.save_trace("execution_trace.txt");
```

## 相關文件

- PE ISA 文檔: `design/hybridacc-pe-isa/doc/ISA.md`
- PE 架構文檔: `design/hybridacc-pe-isa/doc/Hybridacc PE.md`
- PE Simulator 源碼: `design/hybridacc-ESL/simulator/`
- 組譯器工具: `design/hybridacc-pe-isa/src/assembler/`