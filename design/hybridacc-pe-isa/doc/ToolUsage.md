# HybridAcc PE-ISA 工具鏈使用手冊

文件樹： [../../../doc/index.md](../../../doc/index.md) -> [../README.md](../README.md) -> 本頁。

## 目錄
1. [概述](#概述)
2. [工具鏈架構](#工具鏈架構)
3. [組譯器 (ha-asm)](#組譯器-ha-asm)
4. [反組譯器 (ha-objdump)](#反組譯器-ha-objdump)
5. [打包工具 (ha-package)](#打包工具-ha-package)
6. [組合語言語法](#組合語言語法)
7. [Template 模板系統](#template-模板系統)
8. [撰寫限制與注意事項](#撰寫限制與注意事項)
9. [輸出格式說明](#輸出格式說明)
10. [常見錯誤與排除](#常見錯誤與排除)
11. [實例演練](#實例演練)

---

## 概述

HybridAcc PE-ISA 工具鏈提供完整的組合語言開發環境，包含：
- **ha-asm**: 組譯器，將 `.asm` 組合語言轉換為機器碼
- **ha-objdump**: 反組譯器，將機器碼還原為可讀的組合語言
- **ha-package**: 打包工具，將多個 Template 打包成 Package Binary 並生成 C API
- **Template 系統**: 支援參數化組合語言模板，便於批量生成程式碼
- **多種輸出格式**: 支援 Binary (.bin)、Hex (.hex)、JSON (.json)、Disassembly (.disasm)

**指令集特性**:
- 16-bit 固定長度指令
- 支援 DMA 資料搬移、向量運算、迴圈控制、系統指令
- 大小寫不敏感
- 支援 LOOPEND 偽指令 (設定前一指令 bit0)

---

## 工具鏈架構

### 建置工具鏈

```bash
cd /path/to/hybridacc-pe-isa
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
cmake --build . -j
```

編譯完成後，工具位於 `build/src/assambler/`:
- `ha-asm` - 組譯器
- `ha-objdump` - 反組譯器

以及 `build/src/packager/`:
- `ha-package` - 打包工具

### 安裝工具 (可選)

```bash
cd build
cmake --install . --prefix /usr/local
# 或指定自訂路徑
cmake --install . --prefix $HOME/.local
```

### 執行測試

```bash
cd build
ctest --output-on-failure
# 或只測試組譯器
ctest -R assembler
```

---

## 組譯器 (ha-asm)

### 基本用法

```bash
ha-asm <input.asm> [options]
```

### 命令列選項

| 選項 | 說明 | 範例 |
|------|------|------|
| `-o <file.bin>` | 輸出二進制檔案 (Little-endian 16-bit words) | `-o output.bin` |
| `--hex <file.hex>` | 輸出十六進制檔案 (每行一個 0xXXXX) | `--hex output.hex` |
| `--json <file.json\|->` | 輸出 JSON 格式，使用 `-` 輸出到 stdout | `--json output.json` |
| `--disasm <file.disasm>` | 輸出反組譯格式 (僅限 template) | `--disasm output.disasm` |
| `--verbose` | 顯示詳細編譯過程 | `--verbose` |

**預設行為**: 若未指定任何輸出選項，自動生成 `<input>.asm.hex`

### 編譯行為

#### 1. 一般組合語言檔案 (不含 template)

```bash
# 基本編譯
ha-asm conv1d_k3.asm

# 指定多種輸出
ha-asm conv1d_k3.asm -o output.bin --hex output.hex --json output.json

# 詳細模式
ha-asm conv1d_k3.asm --verbose
```

**編譯流程**:
1. 讀取原始碼，移除註解 (以 `#` 開頭的行)
2. 第一遍掃描: 建立標籤表 (label table)
3. 第二遍掃描: 編碼指令，解析操作數
4. 處理 LOOPEND 偽指令與必要的控制序列修正（例如 LDMA.ACT 後自動插入 NOP）
5. 輸出機器碼

#### 2. Template 模板檔案 (含 `.template` 指令)

```bash
# Template 編譯 (自動偵測)
ha-asm template/conv1d_k3s1.asm -o output_temp.bin --hex output_temp.hex --json output_temp.json
```

**編譯流程**:
1. 偵測 `.template` 指令
2. 解析模板標頭: 提取模板名稱和參數定義
3. 編碼指令，記錄需要參數替換的位置 (patches)
4. 輸出包含參數資訊的檔案 (供後續工具使用)

**Template 輸出格式差異**:
- **Binary (.bin)**: 包含 header、patch data、instruction data
- **Hex (.hex)**: 包含註解說明參數與 patches
- **JSON (.json)**: 包含完整的結構化資訊 (template_name, parameters, patches, instructions)
- **Disasm (.disasm)**: 包含參數註解的反組譯碼

---

## 反組譯器 (ha-objdump)

### 基本用法

```bash
ha-objdump <input.bin|input.hex>
```

### 輸入格式

- **Binary (.bin)**: 以 little-endian 16-bit words 讀取
- **Hex (.hex)**: 逐行讀取十六進制值 (可含或不含 `0x` 前綴)

### 輸出格式

```
<index>: <disassembly>
```

範例:
```
0: SDMA.ADDR 0
1: SDMA.LEN 48
2: SDMA.SD 4
3: TSTORE 0
...
```

### 使用範例

```bash
# 反組譯 binary 檔案
ha-objdump output.bin

# 反組譯 hex 檔案
ha-objdump output.hex

# 將反組譯結果導向檔案
ha-objdump output.bin > disasm.txt
```

---

## 打包工具 (ha-package)

### 概述

`ha-package` 是一個專門用於將多個 Template 打包成單一 Package Binary 的工具。它可以：
- 接受多個 `.asm` 或 `.json` 格式的 Template 作為輸入
- 自動編譯 `.asm` Template 文件
- 生成緊湊的 Package Binary 格式
- 產生包含完整資訊的 JSON 文件
- 生成類型安全的 C API Header 文件

### 基本用法

```bash
ha-package [options] <input1> <input2> ...
```

### 命令列選項

| 選項 | 說明 | 範例 |
|------|------|------|
| `-o <file.pkg>` | 輸出 Package Binary 檔案 | `-o kernels.pkg` |
| `--json <file.json>` | 輸出 Package 資訊 JSON | `--json kernels.json` |
| `--header <file.h>` | 輸出 C API Header 檔案 | `--header kernels.h` |
| `--verbose` | 顯示詳細打包過程 | `--verbose` |
| `-h, --help` | 顯示幫助訊息 | `-h` |

**注意**: 至少需要指定一個輸出選項 (`-o`, `--json`, 或 `--header`)

### 輸入格式

ha-package 支援兩種輸入格式：

1. **Template Assembly (.asm)**
   - 包含 `.template` 指令的組合語言文件
   - 工具會自動調用 assembler 進行編譯
   - 適合直接從源碼打包

2. **Template JSON (.json)**
   - 由 `ha-asm` 預先編譯產生的 JSON 文件
   - 可直接打包，無需重新編譯
   - 適合已編譯好的 Template

### Package Binary 格式

Package Binary 採用緊湊的二進制格式：

```
+-------------------+
| Package Header    |
+-------------------+
| Version (8 bits)  |  ← Package 版本號
| Num Templates     |  ← Template 數量 (8 bits)
| Offset 0 (16 bits)|  ← Template 0 在 Binary 中的偏移量
| Offset 1 (16 bits)|  ← Template 1 在 Binary 中的偏移量
| ...               |
+-------------------+
| Template Binary 0 |
+-------------------+
| Template Binary 1 |
+-------------------+
| ...               |
+-------------------+
```

**Header 詳細說明**:
- **Version**: 8-bit，目前版本為 1
- **Num Templates**: 8-bit，支援最多 255 個 Template
- **Offset Vector**: 每個 16-bit，指向對應 Template 在整個 Package 中的位元組偏移量

**Template Binary 格式** (每個 Template 的內部格式):
```
+-------------------+
| Num Patches (8)   |  ← Patch 數量
| Num Instrs (8)    |  ← 指令數量
+-------------------+
| Patch 0           |  ← [offset:8][param_idx:8]
| Patch 1           |
| ...               |
+-------------------+
| Instruction 0     |  ← 16-bit 機器碼 (little-endian)
| Instruction 1     |
| ...               |
+-------------------+
```

### 使用範例

#### 範例 1: 打包單一 Template

```bash
ha-package \
    -o output/my_kernel.pkg \
    --json output/my_kernel.json \
    --header output/my_kernel.h \
    asm/template/conv1d_k3s1.asm
```

#### 範例 2: 打包多個 Template

```bash
ha-package \
    -o output/kernels.pkg \
    --json output/kernels.json \
    --header output/kernels.h \
    asm/template/conv1d_k1s1.asm \
    asm/template/conv1d_k3s1.asm \
    asm/template/conv1d_k5s1.asm \
    asm/template/conv1d_k7s1.asm \
    asm/template/gemv_template.asm
```

#### 範例 3: 混合 ASM 和 JSON 輸入

```bash
# 先編譯一些 template
ha-asm asm/template/conv1d_k3s1.asm --json output/conv1d_k3.json

# 混合已編譯和未編譯的 template
ha-package \
    -o output/mixed.pkg \
    --header output/mixed.h \
    output/conv1d_k3.json \
    asm/template/conv1d_k5s1.asm \
    asm/template/gemv_template.asm
```

#### 範例 4: 使用 Shell Script

專案提供了便利的 shell script (`tools/script/package.sh`)：

```bash
# 直接執行腳本
cd /path/to/hybridacc-pe-isa
./tools/script/package.sh

# 腳本會自動打包 asm/template/ 目錄下的所有 template
# 輸出到 output/package/ 目錄
```

#### 範例 5: Verbose 模式查看詳細資訊

```bash
ha-package --verbose \
    -o output/kernels.pkg \
    --json output/kernels.json \
    --header output/kernels.h \
    asm/template/*.asm
```

**輸出範例**:
```
HybridAcc Package Tool
======================

Loading: asm/template/conv1d_k1s1.asm ... OK (assembled)
Loading: asm/template/conv1d_k3s1.asm ... OK (assembled)
Loading: asm/template/conv1d_k5s1.asm ... OK (assembled)
Loading: asm/template/gemv_template.asm ... OK (assembled)

Creating package...
  Package version: 1
  Number of templates: 4
  Total binary size: 256 bytes

  Template 0: conv1d_k1s1_template
    Offset: 10 bytes
    Parameters: 3
    Patches: 3
    Instructions: 28
  Template 1: conv1d_k3s1_template
    Offset: 72 bytes
    Parameters: 3
    Patches: 4
    Instructions: 32
  ...

Writing binary: output/kernels.pkg ... OK
Writing JSON: output/kernels.json ... OK
Writing header: output/kernels.h ... OK

Package created successfully!
```

### Package JSON 輸出

Package JSON 包含完整的打包資訊，適合程式化處理：

```json
{
  "package_version": 1,
  "num_templates": 4,
  "total_binary_size": 256,
  "templates": [
    {
      "index": 0,
      "name": "conv1d_k3s1_template",
      "offset": 10,
      "binary_size": 74,
      "source": "asm/template/conv1d_k3s1.asm",
      "parameters": [
        {
          "index": 0,
          "name": "KERNEL_DMA_LEN",
          "default": 48
        },
        {
          "index": 1,
          "name": "OUTPUT_WINDOW_CNT",
          "default": 798
        },
        {
          "index": 2,
          "name": "KERNEL_COUNT",
          "default": 16
        }
      ],
      "patches": [
        {
          "offset": 2,
          "param_index": 0
        },
        {
          "offset": 12,
          "param_index": 1
        }
      ],
      "num_instructions": 32
    },
    ...
  ]
}
```

**JSON 欄位說明**:
- `package_version`: Package 格式版本
- `num_templates`: Package 中包含的 Template 數量
- `total_binary_size`: Package Binary 總大小 (bytes)
- `templates[]`: Template 陣列
  - `index`: Template 索引 (0-based)
  - `name`: Template 名稱
  - `offset`: 在 Package Binary 中的位元組偏移量
  - `binary_size`: 該 Template Binary 的大小
  - `source`: 源文件路徑
  - `parameters[]`: 參數定義
  - `patches[]`: Patch 資訊
  - `num_instructions`: 指令數量

### C API Header 輸出

ha-package 生成的 C Header 提供類型安全的 API，包含：

#### 1. 基礎資料結構

```c
// Template patch information
typedef struct {
    uint8_t offset;        // Instruction offset to patch
    uint8_t param_index;   // Parameter index
} ha_template_patch_t;

// Package header structure
typedef struct {
    uint8_t version;        // Package version
    uint8_t num_templates;  // Number of templates
} ha_package_header_t;
```

#### 2. Template 專屬參數結構體

為每個 Template 生成專屬的參數結構體，提供類型安全：

```c
// Parameters for template: conv1d_k3s1_template
typedef struct {
    uint16_t KERNEL_DMA_LEN;       // default: 48
    uint16_t OUTPUT_WINDOW_CNT;    // default: 798
    uint16_t KERNEL_COUNT;         // default: 16
} ha_params_conv1d_k3s1_template_t;

// Parameters for template: gemv_template
typedef struct {
    uint16_t KERNEL_DMA_STORE_LEN; // default: 64
    uint16_t KERNEL_DMA_LOAD_LEN;  // default: 256
    uint16_t INPUT_DIM;            // default: 32
    uint16_t OUTPUT_DIM;           // default: 8
    uint16_t PSUM_COUNT;           // default: 24
} ha_params_gemv_template_t;
```

#### 3. Template 資訊結構

```c
// Template metadata
typedef struct {
    const char* name;              // Template name
    uint8_t template_index;        // Template index in package
    uint16_t offset;               // Offset in package binary
    uint16_t binary_size;          // Size in bytes
    uint8_t num_params;            // Number of parameters
    uint8_t num_patches;           // Number of patches
    uint8_t num_instructions;      // Number of instructions
    const ha_template_patch_t* patches;  // Patch array
    const void* default_params;    // Pointer to default parameters
} ha_template_t;
```

#### 4. Package 資訊定義

```c
// Package information
#define HA_PACKAGE_VERSION 1
#define HA_NUM_TEMPLATES 4
#define HA_TOTAL_BINARY_SIZE 256

// Template indices
#define HA_TEMPLATE_CONV1D_K1S1_TEMPLATE 0
#define HA_TEMPLATE_CONV1D_K3S1_TEMPLATE 1
#define HA_TEMPLATE_CONV1D_K5S1_TEMPLATE 2
#define HA_TEMPLATE_GEMV_TEMPLATE 3
```

#### 5. 預設參數實例

```c
// Default parameters for conv1d_k3s1_template
static const ha_params_conv1d_k3s1_template_t ha_default_params_conv1d_k3s1_template = {
    .KERNEL_DMA_LEN = 48,
    .OUTPUT_WINDOW_CNT = 798,
    .KERNEL_COUNT = 16
};
```

#### 6. Template 資訊表

```c
// Template information table
static const ha_template_t ha_template_table[] = {
    {
        .name = "conv1d_k3s1_template",
        .template_index = 0,
        .offset = 10,
        .binary_size = 74,
        .num_params = 3,
        .num_patches = 4,
        .num_instructions = 32,
        .patches = ha_patches_conv1d_k3s1_template,
        .default_params = &ha_default_params_conv1d_k3s1_template
    },
    ...
};
```

#### 7. Helper 函數

```c
// Get template information by index
static inline const ha_template_t* ha_get_template_info(uint8_t template_index);

// Get template offset in package binary
static inline uint16_t ha_get_template_offset(uint8_t template_index);

// Get template binary size
static inline uint16_t ha_get_template_size(uint8_t template_index);

// Template-specific accessors
static inline const ha_params_conv1d_k3s1_template_t* ha_get_default_params_conv1d_k3s1_template();
static inline const ha_template_t* ha_get_conv1d_k3s1_template_info();
```

### C API 使用範例

#### 範例 1: 取得 Template 資訊

```c
#include "kernels.h"

int main() {
    // 取得 template 資訊
    const ha_template_t* info = ha_get_template_info(HA_TEMPLATE_CONV1D_K3S1_TEMPLATE);

    printf("Template: %s\n", info->name);
    printf("Offset: %u bytes\n", info->offset);
    printf("Size: %u bytes\n", info->binary_size);
    printf("Parameters: %u\n", info->num_params);
    printf("Instructions: %u\n", info->num_instructions);

    return 0;
}
```

#### 範例 2: 使用專屬參數結構體

```c
#include "kernels.h"

void configure_conv1d_kernel() {
    // 使用預設參數
    const ha_params_conv1d_k3s1_template_t* default_params =
        ha_get_default_params_conv1d_k3s1_template();

    printf("Default KERNEL_DMA_LEN: %u\n", default_params->KERNEL_DMA_LEN);
    printf("Default OUTPUT_WINDOW_CNT: %u\n", default_params->OUTPUT_WINDOW_CNT);

    // 或創建自訂參數
    ha_params_conv1d_k3s1_template_t custom_params = {
        .KERNEL_DMA_LEN = 64,
        .OUTPUT_WINDOW_CNT = 1024,
        .KERNEL_COUNT = 32
    };

    // 使用 custom_params 配置硬體...
}
```

#### 範例 3: 載入並解析 Package Binary

```c
#include "kernels.h"
#include <stdio.h>
#include <stdint.h>

void load_package(const char* pkg_path) {
    FILE* f = fopen(pkg_path, "rb");

    // 讀取 package header
    ha_package_header_t header;
    fread(&header, sizeof(header), 1, f);

    printf("Package version: %u\n", header.version);
    printf("Number of templates: %u\n", header.num_templates);

    // 讀取 offset vector
    uint16_t offsets[HA_NUM_TEMPLATES];
    for (int i = 0; i < header.num_templates; i++) {
        fread(&offsets[i], sizeof(uint16_t), 1, f);
        printf("Template %d offset: %u\n", i, offsets[i]);
    }

    // 跳轉到特定 template
    const ha_template_t* info = ha_get_template_info(HA_TEMPLATE_CONV1D_K3S1_TEMPLATE);
    fseek(f, info->offset, SEEK_SET);

    // 讀取 template binary...
    uint8_t template_data[info->binary_size];
    fread(template_data, 1, info->binary_size, f);

    fclose(f);
}
```

### 整合工作流程

完整的 Template 到 Package 工作流程：

```bash
# 步驟 1: 撰寫 Template 源碼
vim asm/template/my_kernel.asm

# 步驟 2: (可選) 單獨編譯測試
ha-asm asm/template/my_kernel.asm --json output/my_kernel.json --verbose

# 步驟 3: 打包所有 Templates
ha-package \
    -o output/package/kernels.pkg \
    --json output/package/kernels.json \
    --header output/package/kernels.h \
    asm/template/*.asm

# 步驟 4: 在 C 程式中使用
gcc my_program.c -I output/package -o my_program
./my_program output/package/kernels.pkg
```

### 最佳實踐

1. **命名規範**
   - Package 文件使用有意義的名稱 (如 `conv_kernels.pkg`, `gemm_kernels.pkg`)
   - Header 文件與 Package 同名 (如 `conv_kernels.h`)

2. **版本管理**
   - 在 JSON 中記錄 source 文件路徑，便於追溯
   - 使用版本控制系統管理 Template 源碼

3. **參數設計**
   - 為 Template 參數提供合理的預設值
   - 在 C Header 中使用語意化的結構體名稱

4. **錯誤處理**
   - 使用 `--verbose` 選項排查打包問題
   - 檢查 JSON 輸出確認 Template 資訊正確

5. **性能優化**
   - 預先編譯常用 Template 為 JSON，避免重複編譯
   - 將相關 Template 打包在同一個 Package 中

### 與其他工具整合

#### Python 整合範例

```python
import json
import struct

# 讀取 Package JSON
with open('output/package/kernels.json', 'r') as f:
    pkg_info = json.load(f)

# 讀取 Package Binary
with open('output/package/kernels.pkg', 'rb') as f:
    # 讀取 header
    version, num_templates = struct.unpack('BB', f.read(2))

    # 讀取 offsets
    offsets = []
    for i in range(num_templates):
        offset, = struct.unpack('<H', f.read(2))
        offsets.append(offset)

    # 載入特定 template
    template_idx = 1
    f.seek(offsets[template_idx])
    # 處理 template binary...
```

---

## 組合語言語法

### 基本規則

1. **大小寫不敏感**: 指令名稱和暫存器名稱不區分大小寫
   ```asm
   VMAC p0, vt0    # 正確
   vmac P0, VT0    # 正確 (同樣效果)
   Vmac p0, Vt0    # 正確 (同樣效果)
   ```

2. **註解**: 使用 `#` 開頭
   ```asm
   # 這是註解
    LDMA.ADDR 0  # 行尾註解
   ```

3. **空白行**: 會被忽略

4. **操作數分隔**: 可使用逗號或空格
   ```asm
   VMAC p0, vt0    # 逗號分隔
   VMAC p0 vt0     # 空格分隔 (不推薦，可能造成混淆)
   ```

### 標籤 (Labels)

目前語法仍可定義標籤（供程式可讀性與後續擴充），但 **ISA v3 不含 `J` 跳轉指令**。

**標籤規則**:
- 標籤以冒號 `:` 結尾
- 必須獨立一行
- 不可重複定義

### 暫存器命名

| 類型 | 名稱格式 | 範圍 | 說明 |
|------|----------|------|------|
| Transform | `T0`~`T11` | 0-11 | 16-bit 轉換暫存器 |
| Vector Transform | `VT0`~`VT2` | 0-2 | 向量轉換暫存器 |
| Partial Sum | `P0`~`P31` | 0-31 | 16-bit 部分和暫存器 |
| Vector Partial Sum | `VP0`~`VP31` | 0-31 | 64-bit 向量部分和暫存器 |

### 立即數格式

```asm
LDMA.ADDR 100        # 十進制
LDMA.ADDR 0x64       # 十六進制 (0x 前綴)
LDMA.ADDR 0X64       # 十六進制 (0X 前綴)
LDMA.ADDR 0b1100100  # 二進制 (0b 前綴)
```

### 特殊 Token

| Token | 用途 | 值 | 說明 |
|-------|------|-----|------|
| `VTRST` | vtstride reset | 3 | Vector transform stride 重置值 (2-bit 最大) |
| `PRST` | pstride reset | 31 | Partial sum stride 重置值 (5-bit 最大) |
| `VPRST` | vpstride reset | 31 | Vector partial sum stride 重置值 |
| `K3` | Kernel size 3 | 3 | TSHIFT 使用 |
| `K5` | Kernel size 5 | 5 | TSHIFT 使用 |
| `K7` | Kernel size 7 | 7 | TSHIFT 使用 |

使用範例:
```asm
VMACRN 1, VTRST     # vtstride = 3, 觸發重置
VMACR PRST, 1       # pstride = 31, 觸發重置
TSHIFT K3           # 使用 kernel size 3
```

### 偽指令

#### LOOPEND

**語法**: `LOOPEND`

**功能**: 標記前一條指令為迴圈結尾

**編碼**: 將前一條機器碼的 bit 0 設為 1

**限制**:
- 不能在檔案第一行使用
- 不能連續使用兩次 LOOPEND

**使用範例**:
```asm
LOOPIN 16
    VMAC p0, vt0
    VMACRN 1, VTRST
    LOOPEND          # 標記 VMACRN 為迴圈結尾

# 反組譯結果:
# 0: LOOPIN 16
# 1: VMAC P0, VT0
# 2: VMACRN PSTR=1, VTSTR=3 ; LOOPEND
```

---

## Template 模板系統

### Template 概述

Template 模板系統允許您撰寫**參數化**的組合語言程式，便於：
- 批量生成不同配置的程式碼
- 減少重複程式碼
- 提高程式可維護性

### Template 語法

#### 1. 模板宣告

```asm
.template
template_name(PARAM1=default1, PARAM2=default2, ...):
    # 模板內容
```

**元素說明**:
- `.template`: 模板指令，必須在檔案開頭
- `template_name`: 模板名稱
- 參數列表:
  - `PARAM=value`: 有預設值的參數
  - `PARAM`: 無預設值的參數 (預設為 0)
- `:`: 冒號結尾

#### 2. 參數使用

在指令中使用 `$(PARAMETER_NAME)` 引用參數:

```asm
LDMA.LEN $(KERNEL_DMA_LEN)
LOOPIN $(OUTPUT_WINDOW_CNT)
```

**支援參數的指令**:
- `LDMA.ADDR $(PARAM)`
- `LDMA.LEN $(PARAM)`
- `SDMA.ADDR $(PARAM)`
- `SDMA.LEN $(PARAM)`
- `LDMA.LOOP $(PARAM)`
- `SDMA.LOOP $(PARAM)`
- `LOOPIN $(PARAM)`

**限制**: 其他指令目前不支援參數替換

### Template 範例

#### 範例 1: 簡單的 1D Convolution Template

```asm
.template
conv1d_k3s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT=798, KERNEL_COUNT=16):
    # Initialize
    SYS.CTRL (CLEAR.P)

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_LEN)  # 參數: kernel DMA 長度
    SDMA.SD 4

preload_input:
    TSTORE t0
    TSTORE t1
    TSTORE t2

loop_window:
    LOOPIN $(OUTPUT_WINDOW_CNT)  # 參數: 滑動視窗數量

    # ... 處理邏輯

    LOOPEND

    HALT
```

#### 範例 2: GEMV Template

```asm
.template
gemv_template(KERNEL_DMA_STORE_LEN=64, KERNEL_DMA_LOAD_LEN=256, INPUT_DIM=32, OUTPUT_DIM=8, PSUM_COUNT=24):
    SYS.CTRL (CLEAR.P)

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN $(KERNEL_DMA_STORE_LEN)
    SDMA.SD 4

    LDMA.ADDR 0
    LDMA.LEN $(KERNEL_DMA_LOAD_LEN)
    LDMA.LHB 1

loop_in_dim:
    LOOPIN $(INPUT_DIM)

    # ... 計算邏輯

    LOOPEND

psum:
    LOOPIN $(PSUM_COUNT)
    VPSUMR 1
    LOOPEND
    SYS.CTRL (CLEAR.P)

    HALT
```

### Template 編譯輸出

#### Binary 格式 (.bin)

```
[Header: 16-bit] [Patch Data...] [Instruction Data...]
```

- **Header**: `[15:8]` patch 數量, `[7:0]` 指令數量
- **Patch Data**: 每個 patch 為 16-bit: `[15:8]` offset, `[7:0]` param_index
- **Instruction Data**: 實際的機器碼指令

#### Hex 格式 (.hex)

```
# Template: conv1d_k3s1_template
# Parameters: 3
#   [0] KERNEL_DMA_LEN = 48
#   [1] OUTPUT_WINDOW_CNT = 798
#   [2] KERNEL_COUNT = 16
# Patches: 2

# Header (patch_len=2, template_len=25)
0x0219

# Patch data (offset, param_index)
0x0100  # offset=1, param=0
0x0801  # offset=8, param=1

# Template instructions (25 words)
0x0002
0x00EA
...
```

#### JSON 格式 (.json)

```json
{
  "template_name": "conv1d_k3s1_template",
  "parameters": [
    {"index": 0, "name": "KERNEL_DMA_LEN", "default": 48},
    {"index": 1, "name": "OUTPUT_WINDOW_CNT", "default": 798},
    {"index": 2, "name": "KERNEL_COUNT", "default": 16}
  ],
  "patches": [
    {"offset": 1, "param_index": 0},
    {"offset": 8, "param_index": 1}
  ],
  "instructions": [
        {"index": 0, "word": "0x008A", "dec": 138, "disasm": "SYSCTRL (CLEAR.P)"},
        {"index": 1, "word": "0x00EA", "dec": 234, "disasm": "LDMA.LEN 48"},
    ...
  ]
}
```

### Template 使用工作流程

1. **撰寫 Template**:
   ```bash
   vim asm/template/my_template.asm
   ```

2. **編譯 Template**:
   ```bash
   ha-asm asm/template/my_template.asm \
       -o output/my_template.bin \
       --hex output/my_template.hex \
       --json output/my_template.json \
       --disasm output/my_template.disasm
   ```

3. **使用 Template 輸出**:
   - Python/C++ 工具讀取 JSON，根據實際參數值進行 patch
   - 載入到模擬器或硬體執行

---

## 撰寫限制與注意事項

### 指令限制

| 限制項目 | 說明 | 範圍/規則 |
|---------|------|-----------|
| **LDMA/SDMA.ADDR** | 10-bit 起始位址 | 0 ~ 1023 |
| **LDMA/SDMA.LEN** | 10-bit 長度 (組語輸入) | 1 ~ 1024（機器碼以 N-1 編碼） |
| **LDMA.L* stride** | 3-bit stride | 0 ~ 7 |
| **SDMA.SD stride** | 3-bit stride | 0 ~ 7 |
| **TSTORE trd** | Transform 暫存器 | 0 ~ 11 (T0-T11) |
| **TSHIFT** | 僅支援 K3/K5/K7 | 3, 5, 7 |
| **VMAC/VMUL prd** | Partial sum 暫存器 | 0 ~ 31 (P0-P31) |
| **vtrs** | Vector transform | 0 ~ 2 (VT0-VT2) |
| **vtstride** | 2-bit stride | 0 ~ 3 (3=VTRST) |
| **pstride** | 5-bit stride | 0 ~ 31 (31=PRST) |
| **vpstride** | 5-bit stride | 0 ~ 31 (31=VPRST) |
| **LOOPIN count** | 10-bit 迴圈計數 | 1 ~ 1024 (機器碼以 N-1 編碼) |
| **LDMA/SDMA.LOOP** | 10-bit 迴圈計數 | 1 ~ 1024 (機器碼以 N-1 編碼) |

### 語法限制

1. **ISA v3 不支援 J 跳轉指令**
    - 目前控制流以 `LOOPIN + LOOPEND` 為主。

2. **LOOPEND 不能是第一條指令**
   ```asm
   # 錯誤
   LOOPEND  # 編譯錯誤: LOOPEND without previous instruction
   ```

3. **Template 參數僅限特定指令**
    - 只有 `LDMA.ADDR`, `LDMA.LEN`, `SDMA.ADDR`, `SDMA.LEN`, `LDMA.LOOP`, `SDMA.LOOP`, `LOOPIN` 支援 `$(PARAM)`
    - 其他指令中使用會被忽略或報錯

4. **SYS.SYNC 只支援 SWAPDM 旗標**
    - `SYS.CTRL (SWAPDM)` 會被視為 `SYS.SYNC (SWAPDM)`
    - `SWAPDM` 不可與其他 SYS.CTRL 旗標混用

### 編碼限制

1. **vtstride 只有 2 bits**
   ```asm
   VMACR 1, 4      # 錯誤: vtstride 超出範圍 (0-3)
   VMACR 1, VTRST  # 正確: VTRST = 3
   ```

2. **未知指令或格式**
   ```asm
   UNKNOWN_INST    # 錯誤: Unknown mnemonic
   ```

### 最佳實踐

1. **使用註解說明意圖**
   ```asm
   # Load 16 kernels, each with 3 vectors
    LDMA.LEN 48  # 16 * 3 = 48
   ```

2. **標籤命名清晰**
   ```asm
   loop_kernel:
   loop_window:
   load_input:
   ```

3. **適當使用 LOOPEND**
   ```asm
   LOOPIN 16
       VMAC p0, vt0
       VMACRN 1, VTRST
       LOOPEND  # 明確標記迴圈結束
   ```

4. **參數命名有意義** (Template)
   ```asm
   # 好的命名
   .template
   conv2d(KERNEL_SIZE=3, INPUT_HEIGHT=32, INPUT_WIDTH=32):

   # 不好的命名
   .template
   conv2d(K=3, H=32, W=32):
   ```

---

## 輸出格式說明

### Binary 格式 (.bin)

- **編碼**: Little-endian 16-bit words
- **用途**: 直接載入硬體或模擬器執行
- **大小**: N 條指令 = N × 2 bytes

**讀取範例 (Python)**:
```python
import struct

with open('output.bin', 'rb') as f:
    while True:
        data = f.read(2)
        if not data:
            break
        word = struct.unpack('<H', data)[0]  # Little-endian uint16
        print(f"0x{word:04X}")
```

### Hex 格式 (.hex)

- **編碼**: 每行一個十六進制值
- **格式**: `0xXXXX` (含前綴) 或 `XXXX`
- **用途**: 人類可讀、腳本處理

**範例**:
```
0x0002
0x00EA
0x1806
0x0C42
```

### JSON 格式 (.json)

- **編碼**: 結構化 JSON 陣列
- **用途**: 程式化處理、可視化、整合工具鏈

**欄位說明**:
- `index`: 指令索引 (從 0 開始)
- `word`: 16-bit 機器碼 (十六進制字串)
- `dec`: 機器碼的十進制值
- `disasm`: 反組譯字串

**範例**:
```json
[
    {"index": 0, "word": "0x008A", "dec": 138, "disasm": "SYSCTRL (CLEAR.P)"},
    {"index": 1, "word": "0x00EA", "dec": 234, "disasm": "LDMA.LEN 48"},
    {"index": 2, "word": "0x1806", "dec": 6150, "disasm": "SDMA.SD 4"}
]
```

### Disassembly 格式 (.disasm)

- **編碼**: 純文字，每行一條指令
- **格式**: `<index>: <disasm>`
- **用途**: 除錯、比對、文件

**範例**:
```
    0: SYSCTRL (CLEAR.P)
    1: LDMA.LEN 48
    2: SDMA.SD 4
   3: TSTORE 0
   4: TSTORE 1
```

---

## 常見錯誤與排除

### 編譯錯誤

| 錯誤訊息 | 原因 | 解決方法 |
|---------|------|----------|
| `Unknown mnemonic 'XXX'` | 指令名稱錯誤或拼寫錯誤 | 檢查指令名稱，參考 ISA 文件 |
| `Duplicate label 'XXX'` | 重複定義標籤 | 確保每個標籤只定義一次 |
| `* out of range` | 操作數超出範圍 | 檢查立即數或暫存器編號範圍 |
| `LOOPEND without previous instruction` | LOOPEND 在檔案開頭或連續使用 | 確保 LOOPEND 前有指令 |
| `expects N operands` | 操作數數量錯誤 | 檢查指令語法，補齊或移除多餘操作數 |
| `Cannot open input` | 檔案不存在或無讀取權限 | 檢查檔案路徑和權限 |
| `Undefined template parameter` | Template 中使用未定義參數 | 檢查參數名稱拼寫 |

### 執行錯誤

| 問題 | 可能原因 | 解決方法 |
|------|---------|----------|
| 程式無限迴圈 | LOOPEND 位置錯誤 | 檢查 LOOPEND 是否在正確位置 |
| 記憶體存取錯誤 | LDMA/SDMA.ADDR 超出範圍 | 確認位址在 0-1023 範圍內 |
| 計算結果錯誤 | 暫存器索引錯誤 | 檢查 P/VP/T/VT 暫存器編號 |

### 除錯技巧

1. **使用 --verbose 選項**
   ```bash
   ha-asm input.asm --verbose
   ```
   輸出會顯示每條指令的編譯過程和標籤表。

2. **檢查反組譯結果**
   ```bash
   ha-asm input.asm -o output.bin
   ha-objdump output.bin
   ```
   比對原始碼和反組譯結果，確認編譯正確。

3. **使用 JSON 格式檢查**
   ```bash
   ha-asm input.asm --json - | jq .
   ```
   JSON 格式更容易檢查機器碼和反組譯。

4. **逐步測試**
   - 先註解掉部分程式碼
   - 逐步加入指令並測試
   - 找出問題指令

---

## 實例演練

### 範例 1: 簡單的向量加法

**目標**: 計算 4 個向量的 MAC 運算

```asm
# Simple vector MAC example
# Compute: result = sum(vector_i * weight_i) for i=0..3

init:
    SYS.CTRL (CLEAR.P)   # 清除部分和暫存器

load_data:
    LDMA.ADDR 0          # 設定 LDMA 起始位址
    LDMA.LEN 64          # 載入 64 個資料
    LDMA.LD 1            # 載入 double-word, stride=1

compute:
    SYS.CTRL (RST.PID, RST.TID)

    # MAC 運算
    VMAC p0, vt0         # p0 += vt0 * dmrv
    VMAC p1, vt1         # p1 += vt1 * dmrv
    VMAC p2, vt2         # p2 += vt2 * dmrv
    VMACN p3, vt0        # p3 += vt0 * dmrv, 觸發下一次 DMA

accumulate:
    VPSUM vp0            # 累加到輸出
    SYS.CTRL (CLEAR.P)   # 清除暫存器

done:
    HALT                 # 結束程式
```

**編譯與執行**:
```bash
# 編譯
ha-asm vector_add.asm -o vector_add.bin --hex vector_add.hex --json vector_add.json

# 檢查反組譯
ha-objdump vector_add.bin

# 查看 JSON (需要安裝 jq)
jq . vector_add.json
```

### 範例 2: 含迴圈的 1D Convolution

**目標**: 使用迴圈處理多個滑動視窗

```asm
# 1D Convolution with kernel size 3
# Process 800 input elements with 16 kernels

conv1d_k3:
    SYS.CTRL (CLEAR.P)

load_kernel:
    SDMA.ADDR 0
    SDMA.LEN 48          # 16 kernels × 3 vectors = 48
    SDMA.SD 4            # 儲存 kernel 資料

preload_input:
    TSTORE t0
    TSTORE t3
    TSTORE t6

loop_start:
    LOOPIN 800           # 處理 800 個滑動視窗

window_process:
    TSTORE t1            # 載入新的輸入
    TSTORE t4
    TSTORE t7

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4
    SYS.CTRL (RST.PID, RST.TID)

kernel_loop:
    LOOPIN 16            # 16 個 kernels
    VMACRN 0, 1          # MAC with stride
    VMACRN 0, 1
    VMACRN 1, VTRST      # 最後一次, 重置 VTID
    LOOPEND              # kernel_loop 結束

output:
    VPSUM vp0
    VPSUM vp1
    VPSUM vp2
    VPSUM vp3
    SYS.CTRL (CLEAR.P)

    TSHIFT K3            # Shift kernel
    LOOPEND              # loop_start 結束

finish:
    HALT
```

**編譯與測試**:
```bash
ha-asm conv1d_k3.asm --verbose -o conv1d.bin --json conv1d.json
ha-objdump conv1d.bin > conv1d.disasm
```

### 範例 3: 使用 Template

**Template 定義** (`template/gemv_simple.asm`):
```asm
##############################################################################
# GEMV Template
# Performs General Matrix-Vector multiplication
##############################################################################
.template
gemv_simple(M=32, N=16, BATCH=8):
    SYS.CTRL (CLEAR.P)

load_matrix:
    SDMA.ADDR 0
    SDMA.LEN $(M)        # 使用參數 M
    SDMA.SD 4

load_vector:
    LDMA.ADDR 0
    LDMA.LEN $(N)        # 使用參數 N
    LDMA.LHB 1

compute_loop:
    LOOPIN $(M)          # 使用參數 M

    VMULR 1, 1
    VMULRN 1, VTRST

    LOOPEND

accumulate:
    LOOPIN $(BATCH)      # 使用參數 BATCH
    VPSUMR 1
    LOOPEND

    SYS.CTRL (CLEAR.P)
    HALT
```

**編譯 Template**:
```bash
ha-asm template/gemv_simple.asm \
    -o output/gemv_simple.bin \
    --hex output/gemv_simple.hex \
    --json output/gemv_simple.json \
    --disasm output/gemv_simple.disasm
```

**查看 Template 資訊**:
```bash
# 查看參數定義和 patches
jq '.parameters, .patches' output/gemv_simple.json

# 輸出範例:
# [
#   {"index": 0, "name": "M", "default": 32},
#   {"index": 1, "name": "N", "default": 16},
#   {"index": 2, "name": "BATCH", "default": 8}
# ]
# [
#   {"offset": 2, "param_index": 0},
#   {"offset": 5, "param_index": 1},
#   {"offset": 7, "param_index": 0},
#   {"offset": 12, "param_index": 2}
# ]
```

### 範例 4: 系統控制與同步

```asm
start:
    SYS.CTRL (CLEAR.P, CLEAR.T, RST.PID, RST.TID)
    SDMA.ADDR 0
    SDMA.LEN 48
    SDMA.SD 4
    SYS.CTRL (SDMA.ACT)

    LDMA.ADDR 0
    LDMA.LEN 48
    LDMA.LD 4
    SYS.CTRL (LDMA.ACT)

wait_and_swap:
    SYS.SYNC (SWAPDM)
    HALT
```

**注意**:
- `SYS.SYNC (SWAPDM)` 在 simulator 中會等待 SDMA 完成後才交換 DM bank。
- `SWAPDM` 不可與其他 `SYS.CTRL` 旗標同時出現。

---

## 附錄

### A. 指令快速參考

完整指令集請參考:
- **doc/ISA.md**: 簡化指令集摘要
- **doc/HybridaccPE.md**: 完整指令集與硬體架構文件
- **README.md**: 快速開始指南

### B. 工具鏈檔案結構

```
hybridacc-pe-isa/
├── asm/                      # 組合語言範例
│   ├── conv1d_k1.asm
│   ├── conv1d_k3.asm
│   ├── gemv.asm
│   └── template/             # Template 範例
│       ├── conv1d_k3s1.asm
│       └── gemv_template.asm
├── src/
│   └── assambler/            # 組譯器源碼
│       ├── ha_asm.cpp        # 組譯器主程式
│       ├── ha_objdump.cpp    # 反組譯器主程式
│       ├── instruction.cpp   # 指令編碼/解碼
│       └── utils.cpp         # I/O 工具
├── doc/                      # 文件
│   ├── ISA.md               # 指令集摘要
│   ├── Hybridacc PE.md      # 完整文件
│   └── ToolUsage.md         # 本文件
├── output/                   # 編譯輸出
└── tests/                    # 單元測試
```

### C. 相關資源

- **建置系統**: CMake 3.15+
- **編譯器**: C++17 支援
- **測試框架**: CTest

### D. 版本歷史

| 版本 | 日期 | 變更內容 |
|------|------|----------|
| 1.0 | 2025-08 | 初始版本，DMA.ADDR/LEN 10-bit 編碼 |
| 1.1 | 2025-12 | 新增 Template 系統支援 |
| 1.2 | 2026-01 | 更新 LOOP 編碼原則 (0-based 映射到 1..N) |
| 1.3 | 2026-02 | 對齊 ISA v3：LDMA/SDMA 指令族、SYS.CTRL/SYS.SYNC、移除 J 範例 |

---

## 聯絡與支援

如有問題或建議，請參考:
- 專案 README.md
- 提交 Issue 或 Pull Request
- 查閱 doc/ 目錄下的詳細文件

**祝您使用愉快！**
