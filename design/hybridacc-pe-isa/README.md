# HybridAcc PE-ISA 工具套件

本目錄提供 HybridAcc PE 16-bit ISA 的完整工具鏈：
- **C++ 組譯器 (ha-asm)**: 組合語言轉機器碼，支援 Template 模板系統
- **C++ 反組譯器 (ha-objdump)**: 機器碼還原為組合語言
- **C++ 打包工具 (ha-package)**: 將多個 Template 打包成 Package Binary，並生成 C API Header
- **靜態函式庫 (hybridacc_asm)**: 包含 Assembler / Disassembler 類別，可供其他程式使用
- **單元測試**: 基礎測試案例 (tests/test_assembler.cpp)

---

## 📚 文檔導覽

| 文檔 | 說明 | 適用對象 |
|------|------|----------|
| **[ToolUsage.md](doc/ToolUsage.md)** | 🔧 **工具鏈使用手冊** - 詳細的編譯、Template、撰寫限制與實例 | 組合語言開發者 |
| **[ISA.md](doc/ISA.md)** | 📋 **指令集快速參考** - 簡化的指令編碼與欄位說明 | 需要快速查詢指令格式 |
| **[HybridaccPE.md](doc/HybridaccPE.md)** | 📖 **完整規格文件** - PE 架構與指令集完整說明 | 硬體設計者、架構師 |

**推薦閱讀順序**:
1. 本 README (快速上手)
2. [ToolUsage.md](doc/ToolUsage.md) (詳細使用說明)
3. [ISA.md](doc/ISA.md) 或 [HybridaccPE.md](doc/HybridaccPE.md) (深入了解指令集)

---

## 🚀 快速開始

### 建置工具鏈

```bash
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
cmake --build . -j
ctest --output-on-failure  # 執行測試 (可選)
```

產生的執行檔：
- `build/src/assambler/ha-asm` - 組譯器
- `build/src/assambler/ha-objdump` - 反組譯器
- `build/src/packager/ha-package` - 打包工具

### 安裝 (可選)

```bash
cmake --install . --prefix <安裝路徑>
# 例如:
# cmake --install . --prefix /usr/local
# cmake --install . --prefix $HOME/.local
```

---

## 🔨 基本使用

### 組譯器 (ha-asm)

```bash
# 基本編譯 (自動生成 .hex)
ha-asm input.asm

# 指定輸出格式
ha-asm input.asm -o output.bin          # 二進制
ha-asm input.asm --hex output.hex       # 十六進制
ha-asm input.asm --json output.json     # JSON 格式
ha-asm input.asm --verbose              # 詳細模式

# 多種輸出
ha-asm input.asm -o output.bin --hex output.hex --json output.json
```

**輸出格式說明**:
- **Binary (.bin)**: Little-endian 16-bit words，可直接載入硬體/模擬器
- **Hex (.hex)**: 每行一個 `0xXXXX`，人類可讀
- **JSON (.json)**: 結構化格式，包含機器碼、反組譯、索引等資訊

**JSON 格式範例**:
```json
[
  {"index":0, "word":"0x0002", "dec":2, "disasm":"NOP"},
  {"index":1, "word":"0xE00E", "dec":57358, "disasm":"HALT"}
]
```

**JSON 欄位說明**:
- `index`: 指令序號 (以 16-bit word 為單位)
- `word`: 16-bit 機器碼 (十六進位字串)
- `dec`: 十進制整數值
- `disasm`: 反組譯字串 (若 bit0=1 會附加 `; LOOPEND`)

### 反組譯器 (ha-objdump)

```bash
ha-objdump <input.bin|input.hex>
```

- **.bin**: 以 little-endian 16-bit 讀取
- **其他**: 視為 hex 列表 (可含 0x 前綴)

**輸出格式**:
```
<index>: <disasm>
```

範例:
```
0: LDMA.ADDR 0
1: LDMA.LEN 48
2: SDMA.SD 4
3: TSTORE 0
```

### 打包工具 (ha-package)

將多個 Template 打包成單一 Package Binary，並生成 JSON 資訊和 C Header API。

```bash
# 基本用法
ha-package [options] <input1.asm> <input2.json> ...

# 完整範例
ha-package \
    -o output/kernel.pkg \
    --json output/kernel.json \
    --header output/kernel.h \
    --verbose \
    asm/template/*.asm

# 使用 shell script
tools/script/package.sh
```

**命令列選項**:
- `-o <file.pkg>`: 輸出 package binary 檔案
- `--json <file.json>`: 輸出 package 資訊 JSON
- `--header <file.h>`: 輸出 C API header 檔案
- `--verbose`: 顯示詳細打包過程

**輸入格式**:
- `.asm` 檔案: Template 組合語言 (自動編譯)
- `.json` 檔案: 已編譯的 Template JSON (由 ha-asm 產生)

**Package Binary 格式**:
```
Header:
  [package version,  8 bits]
  [num of templates, 8 bits]
  [offset vector 0,  16 bits]
  [offset vector 1,  16 bits]
  ...
Body:
  [template binary 0]
  [template binary 1]
  ...
```

**生成的 C Header 包含**:
- 每個 Template 的專屬參數結構體 (類型安全)
- Package 資訊 (版本、Template 數量、總大小)
- Template 索引定義
- Template 資訊表
- Helper 函數 (取得 Template 資訊、參數等)

**C Header 範例**:
```c
// Template-specific parameter struct
typedef struct {
    uint16_t KERNEL_DMA_LEN;       // default: 48
    uint16_t OUTPUT_WINDOW_CNT;    // default: 798
    uint16_t KERNEL_COUNT;         // default: 16
} ha_params_conv1d_k3s1_template_t;

// Template metadata
typedef struct {
    const char* name;
    uint8_t template_index;
    uint16_t offset;
    uint16_t binary_size;
    uint8_t num_params;
    uint8_t num_patches;
    uint8_t num_instructions;
    const ha_template_patch_t* patches;
    const void* default_params;
} ha_template_t;

// Template table
static const ha_template_t ha_template_table[] = { ... };

// Helper functions
const ha_template_t* ha_get_template_info(uint8_t template_index);
uint16_t ha_get_template_offset(uint8_t template_index);
```

**詳細 Package 使用說明請參閱**: [ToolUsage.md](doc/ToolUsage.md) 的 "Package 打包系統" 章節

---

## 📝 組合語言特性

### 基本語法

- **大小寫不敏感** (mnemonic/暫存器)
- **註解**: 以 `#` 開頭
- **偽指令 LOOPEND**: 將前一條指令 bit0 設為 1

**範例**:
```asm
# 1D Convolution example
conv1d_k3:
    SYS.CTRL PZERO       # 清除 P 路徑索引

load_kernel:
    LDMA.ADDR 0          # 設定 DMA 位址
    LDMA.LEN 48          # 載入 48 個資料
    SDMA.SD 4

loop_start:
    LOOPIN 16            # 迴圈 16 次
    VMACRN 0, 1
    VMACRN 1, VTRST      # VTRST = 3 (reset)
    LOOPEND              # 標記迴圈結束

    HALT
```

### 特殊 Token

| Token | 值 | 用途 |
|-------|-----|------|
| `VTRST` | 3 | Vector transform stride reset (2-bit 最大值) |
| `PRST` / `VPRST` | 31 | Partial sum stride reset (5-bit 最大值) |
| `K3` / `K5` / `K7` | 0/1/2 (編碼) | TSHIFT kernel size token |

---

## 🎯 Template 模板系統

Template 支援**參數化組合語言**，便於批量生成不同配置的程式碼。

### Template 語法

```asm
.template
template_name(PARAM1=default1, PARAM2=default2, ...):
    # 使用 $(PARAM) 引用參數
    LDMA.LEN $(PARAM1)
    LOOPIN $(PARAM2)
    # ... 其他指令
```

### 範例

```asm
.template
conv1d_k3s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT=798, KERNEL_COUNT=16):
    SYS.CTRL PZERO

load_kernel:
    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LEN)  # 使用參數
    SDMA.SD 4

loop_window:
    LOOPIN $(OUTPUT_WINDOW_CNT)  # 使用參數
    # ... 處理邏輯
    LOOPEND

    HALT
```

### Template 編譯

```bash
ha-asm template/my_template.asm \
    -o output_temp.bin \
    --hex output_temp.hex \
    --json output_temp.json \
    --disasm output_temp.disasm
```

**Template 輸出包含**:
- 參數定義 (名稱、預設值)
- Patch 位置 (哪些指令需要參數替換)
- 機器碼指令

**詳細 Template 使用說明請參閱**: [ToolUsage.md](doc/ToolUsage.md) 的 "Template 模板系統" 章節

---

## 📋 指令集摘要

### 資料搬移 (Data Movement)

| 指令 | 說明 | 範圍 |
|------|------|------|
| `LDMA.ADDR start_addr` | 設定 load DMA 起始位址 | 0..1023 (10-bit) |
| `LDMA.LEN len` | 設定 load DMA 長度 | 1..1024（組語） |
| `LDMA.L[B/H/W/D/BB/HB/WB] stride` | 載入資料 (各種型別/廣播) | stride: 0..7 |
| `SDMA.ADDR start_addr` | 設定 store DMA 起始位址 | 0..1023 (10-bit) |
| `SDMA.LEN len` | 設定 store DMA 長度 | 1..1024（組語） |
| `SDMA.S[B/H/W/D/BB/HB/WB] stride` | 儲存資料 (各種型別/廣播) | stride: 0..7 |
| `TSTORE trd` | 儲存到 Transform 暫存器 | trd: 0..11 |
| `TSHIFT K3/K5/K7` | Kernel shift | 3, 5, 7 |

**編碼細節**: `*.ADDR` / `*.LEN` 使用 10-bit 壓縮編碼
- `value[9:7]` → bits[15:13]
- `value[0]` → bit[11]
- `value[6:1]` → bits[10:5]
- func1 (bit[12]): 0=ADDR, 1=LEN
- `LEN/LOOP/LOOPIN` 在機器碼中採用 `(N-1)` 編碼

### 算術運算 (Arithmetic)

所有算術指令使用 `opcode=10 funct2=01`

| 指令 | 說明 | 操作數 |
|------|------|--------|
| `VMAC / VMACN prd, vtrs` | MAC 運算 (N=觸發下一次 DMA) | prd: 0..31, vtrs: 0..2 |
| `VMACR / VMACRN pstride, vtstride` | MAC + stride 控制 | pstride: 0..31, vtstride: 0..3 |
| `VMUL / VMULN prd, vtrs` | 乘法 | 同上 |
| `VMULR / VMULRN vpstride, vtstride` | 乘法 + stride 控制 | 同上 |
| `VPSUM vprs` | 部分和累加 | vprs: 0..31 |
| `VPSUMR vpstride` | 部分和累加 + stride | vpstride: 0..31 |

**Reset 條件**:
- `pstride == 31` 或 `vpstride == 31`: 重置 PID
- `vtstride == 3`: 重置 VTID (2-bit 編碼最大值)

### 控制流程 (Control Flow)

| 指令 | 說明 | 範圍 |
|------|------|------|
| `LOOPIN count` | 推入迴圈計數 | 1..1024（組語，機器碼為 N-1） |
| `LOOPBREAK` | 結束迴圈（模擬器 decode 路徑） | - |
| `LOOPEND` (偽指令) | 標記前一指令為迴圈結尾 | - |

### 系統指令 (System)

| 指令 | 說明 |
|------|------|
| `NOP` | 無操作 |
| `SYS.CTRL <flags>` | 設定/清除 RID 與 reset 旗標（如 `TSET/PSET/PTSET/TZERO/PZERO`） |
| `SYS.SYNC SWAPDM` | 切換 DMA bank（具等待條件） |
| `HALT` | 停止執行 |

**完整指令編碼請參考**: [ISA.md](doc/ISA.md) 或 [HybridaccPE.md](doc/HybridaccPE.md)

---

## ⚠️ 撰寫限制與注意事項

### 重要限制

1. **`*.ADDR` 欄位**: 僅支援 10-bit 值 (0..1023)
2. **`LEN/LOOP/LOOPIN`**: 組語輸入為 N、機器碼儲存為 `(N-1)`
3. **`vtstride`**: 僅 2 bits (0..3)，使用 3 作為 reset 條件
4. **`LOOPEND`**: 只會修改前一條指令 bit0，不能單獨作為第一條指令
5. **Template 參數**: 僅支援 `LDMA/SDMA` 的 `ADDR/LEN/LOOP` 與 `LOOPIN`

### 常見錯誤

| 錯誤訊息 | 說明 |
|---------|------|
| `Unknown mnemonic` | 指令名稱錯誤或未實作 |
| `Duplicate label` | 重複標籤定義 |
| `* out of range` | 操作數超過欄位可表達範圍 |
| `LOOPEND without previous instruction` | 檔案第一行或連續 LOOPEND |

**詳細限制與最佳實踐請參閱**: [ToolUsage.md](doc/ToolUsage.md)

---

## 🧪 測試

執行單元測試:
```bash
cd build
ctest --output-on-failure

# 或只測試組譯器
ctest -R assembler

# 直接執行測試程式
./tests/test_assembler
```

---

## 📁 專案結構

```
hybridacc-pe-isa/
├── README.md                 # 本文件 (快速入口)
├── CMakeLists.txt            # 建置設定
├── asm/                      # 組合語言範例
│   ├── conv1d_k1.asm
│   ├── conv1d_k3.asm
│   ├── conv1d_k5.asm
│   ├── conv1d_k7.asm
│   ├── gemv.asm
│   └── template/             # Template 範例
│       ├── conv1d_k1s1.asm
│       ├── conv1d_k3s1.asm
│       ├── conv1d_k5s1.asm
│       ├── conv1d_k7s1.asm
│       └── gemv_template.asm
├── src/
│   ├── assambler/            # 組譯器源碼
│   │   ├── ha_asm.cpp        # 組譯器主程式
│   │   ├── ha_objdump.cpp    # 反組譯器主程式
│   │   ├── instruction.cpp   # 指令編碼/解碼邏輯
│   │   ├── instruction.hpp   # 指令定義
│   │   ├── utils.cpp         # I/O 工具函式
│   │   └── utils.hpp
│   ├── packager/             # 打包工具源碼
│   │   ├── ha-package.cpp    # 打包工具主程式
│   │   ├── package.cpp       # Package 邏輯實作
│   │   └── package.hpp       # Package 資料結構
│   └── simulator/            # PE 模擬器
├── doc/                      # 文檔
│   ├── ToolUsage.md          # 工具鏈使用手冊 (詳細)
│   ├── ISA.md                # 指令集快速參考
│   └── HybridaccPE.md        # 完整 PE 架構與指令集文件
├── tools/
│   ├── bin/                  # 工具連結 (由安裝腳本建立)
│   └── script/
│       └── package.sh        # Package 打包腳本
├── output/                   # 編譯輸出目錄
│   └── package/              # Package 輸出
│       ├── kernel.pkg        # Package binary
│       ├── kernel.json       # Package 資訊
│       └── kernel.h          # C API header
├── tests/                    # 單元測試
│   └── test_assembler.cpp
└── build/                    # 建置目錄 (由 CMake 生成)
```

---

## 🔗 整合建議

### JSON 介面整合

工具鏈腳本可用 `ha-asm prog.asm --json -` 直接讀取 stdout JSON，以便後續載入模擬器或可視化:

```bash
# 輸出到 stdout (使用 jq 處理)
ha-asm program.asm --json - | jq .

# Python 整合範例
json_output=$(ha-asm program.asm --json -)
python simulator.py --input "$json_output"
```

### C++ 函式庫整合

可以直接使用靜態函式庫 `libhybridacc_asm.a`:

```cpp
#include "instruction.hpp"

using namespace hybridacc;

// 組譯
Assembler asm;
auto words = asm.assemble(source_code, verbose);

// 反組譯
Disassembler disasm;
std::string disasm_str = disasm.disasmWord(word);
```

---

## 🛠️ 開發與貢獻

### 新增指令流程

1. 更新 `doc/HybridaccPE.md` / `doc/ISA.md`
2. 在 `src/assambler/instruction.cpp` 中新增編碼/解碼邏輯
3. 補充測試案例 (`tests/test_assembler.cpp`)
4. 執行 `ctest` 確保測試通過

### 編譯要求

- **CMake**: 3.15+
- **C++ 編譯器**: 支援 C++17 標準
- **建議**: GCC 7+, Clang 5+, MSVC 2019+

---

## 📞 支援與回饋

如有問題或建議:
- 查閱 [ToolUsage.md](doc/ToolUsage.md) 詳細說明
- 查閱 [ISA.md](doc/ISA.md) 或 [HybridaccPE.md](doc/HybridaccPE.md) 指令集文件
- 提交 Issue 或 Pull Request
- 聯絡專案維護者

---

## 📜 授權

(依專案主專案授權規範填寫)

---

**祝您使用愉快！如需詳細使用說明，請參閱 [ToolUsage.md](doc/ToolUsage.md)**
