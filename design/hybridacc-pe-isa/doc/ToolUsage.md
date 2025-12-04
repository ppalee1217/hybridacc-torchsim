# HybridAcc PE-ISA 工具鏈使用手冊

## 目錄
1. [概述](#概述)
2. [工具鏈架構](#工具鏈架構)
3. [組譯器 (ha-asm)](#組譯器-ha-asm)
4. [反組譯器 (ha-objdump)](#反組譯器-ha-objdump)
5. [組合語言語法](#組合語言語法)
6. [Template 模板系統](#template-模板系統)
7. [撰寫限制與注意事項](#撰寫限制與注意事項)
8. [輸出格式說明](#輸出格式說明)
9. [常見錯誤與排除](#常見錯誤與排除)
10. [實例演練](#實例演練)

---

## 概述

HybridAcc PE-ISA 工具鏈提供完整的組合語言開發環境，包含：
- **ha-asm**: 組譯器，將 `.asm` 組合語言轉換為機器碼
- **ha-objdump**: 反組譯器，將機器碼還原為可讀的組合語言
- **Template 系統**: 支援參數化組合語言模板，便於批量生成程式碼
- **多種輸出格式**: 支援 Binary (.bin)、Hex (.hex)、JSON (.json)、Disassembly (.disasm)

**指令集特性**:
- 16-bit 固定長度指令
- 支援 DMA 資料搬移、向量運算、迴圈控制、系統指令
- 大小寫不敏感
- 支援標籤 (Label) 與跳轉

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
4. 套用標籤修正 (patch): 將 J 指令中的標籤替換為實際位址
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
0: DMA.ADDR 0
1: DMA.LEN 48
2: DMA.SD 4
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
   DMA.ADDR 0  # 行尾註解
   ```

3. **空白行**: 會被忽略

4. **操作數分隔**: 可使用逗號或空格
   ```asm
   VMAC p0, vt0    # 逗號分隔
   VMAC p0 vt0     # 空格分隔 (不推薦，可能造成混淆)
   ```

### 標籤 (Labels)

**定義標籤**:
```asm
loop_start:
    VMAC p0, vt0
    # ... 其他指令
    J loop_start    # 跳轉回 loop_start
```

**標籤規則**:
- 標籤以冒號 `:` 結尾
- 必須獨立一行
- 不可重複定義
- 僅支援 `J` 指令使用 (絕對位址跳轉)

**標籤位址計算**:
- 標籤指向的是**指令索引** (instruction index)
- J 指令使用**位元組位址** (byte address = instruction_index × 2)
- 必須 2-byte 對齊

### 暫存器命名

| 類型 | 名稱格式 | 範圍 | 說明 |
|------|----------|------|------|
| Transform | `T0`~`T11` | 0-11 | 16-bit 轉換暫存器 |
| Vector Transform | `VT0`~`VT2` | 0-2 | 向量轉換暫存器 |
| Partial Sum | `P0`~`P31` | 0-31 | 16-bit 部分和暫存器 |
| Vector Partial Sum | `VP0`~`VP31` | 0-31 | 64-bit 向量部分和暫存器 |

### 立即數格式

```asm
DMA.ADDR 100        # 十進制
DMA.ADDR 0x64       # 十六進制 (0x 前綴)
DMA.ADDR 0X64       # 十六進制 (0X 前綴)
DMA.ADDR 0b1100100  # 二進制 (0b 前綴)
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
DMA.LEN $(KERNEL_DMA_LEN)
LOOPIN $(OUTPUT_WINDOW_CNT)
```

**支援參數的指令**:
- `DMA.ADDR $(PARAM)`
- `DMA.LEN $(PARAM)`
- `LOOPIN $(PARAM)`

**限制**: 其他指令目前不支援參數替換

### Template 範例

#### 範例 1: 簡單的 1D Convolution Template

```asm
.template
conv1d_k3s1_template(KERNEL_DMA_LEN=48, OUTPUT_WINDOW_CNT=798, KERNEL_COUNT=16):
    # Initialize
    CLEAR.P

load_kernel:
    DMA.ADDR 0
    DMA.LEN $(KERNEL_DMA_LEN)  # 參數: kernel DMA 長度
    DMA.SD 4

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
    CLEAR.P

load_kernel:
    DMA.ADDR 0
    DMA.LEN $(KERNEL_DMA_STORE_LEN)
    DMA.SD 4

    DMA.ADDR 0
    DMA.LEN $(KERNEL_DMA_LOAD_LEN)
    DMA.LHB 1

loop_in_dim:
    LOOPIN $(INPUT_DIM)

    # ... 計算邏輯

    LOOPEND

psum:
    LOOPIN $(PSUM_COUNT)
    VPSUMR 1
    LOOPEND
    CLEAR.P

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
    {"index": 0, "word": "0x0002", "dec": 2, "disasm": "CLEAR.P"},
    {"index": 1, "word": "0x00EA", "dec": 234, "disasm": "DMA.LEN 48"},
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
| **DMA.ADDR** | 10-bit 起始位址 | 0 ~ 1023 |
| **DMA.LEN** | 10-bit 長度 | 0 ~ 1023 |
| **DMA.L* stride** | 3-bit stride | 0 ~ 7 |
| **DMA.SD stride** | 3-bit stride | 0 ~ 7 |
| **TSTORE trd** | Transform 暫存器 | 0 ~ 11 (T0-T11) |
| **TSHIFT** | 僅支援 K3/K5/K7 | 3, 5, 7 |
| **VMAC/VMUL prd** | Partial sum 暫存器 | 0 ~ 31 (P0-P31) |
| **vtrs** | Vector transform | 0 ~ 2 (VT0-VT2) |
| **vtstride** | 2-bit stride | 0 ~ 3 (3=VTRST) |
| **pstride** | 5-bit stride | 0 ~ 31 (31=PRST) |
| **vpstride** | 5-bit stride | 0 ~ 31 (31=VPRST) |
| **LOOPIN count** | 10-bit 迴圈計數 | 0 ~ 1023 |
| **J immediate** | 11-bit 位元組位址 | 0 ~ 2047, **必須 2-byte 對齊** |
| **J label** | 標籤跳轉 | 目標必須在程式範圍內 |

### 語法限制

1. **標籤只能用於 J 指令**
   ```asm
   # 正確
   J loop_start

   # 錯誤: LOOPIN 不支援標籤
   LOOPIN loop_start  # 編譯錯誤
   ```

2. **LOOPEND 不能是第一條指令**
   ```asm
   # 錯誤
   LOOPEND  # 編譯錯誤: LOOPEND without previous instruction
   ```

3. **不支援相對跳轉**
   - J 指令使用絕對位址 (指令索引)
   - 無法使用 `J +10` 或 `J -5` 等相對跳轉

4. **Template 參數僅限特定指令**
   - 只有 `DMA.ADDR`, `DMA.LEN`, `LOOPIN` 支援 `$(PARAM)`
   - 其他指令中使用會被忽略或報錯

### 編碼限制

1. **vtstride 只有 2 bits**
   ```asm
   VMACR 1, 4      # 錯誤: vtstride 超出範圍 (0-3)
   VMACR 1, VTRST  # 正確: VTRST = 3
   ```

2. **J 位址必須對齊**
   ```asm
   J 100  # 正確: 100 為偶數
   J 101  # 錯誤: J immediate must be even
   ```

3. **未知指令或格式**
   ```asm
   UNKNOWN_INST    # 錯誤: Unknown mnemonic
   ```

### 最佳實踐

1. **使用註解說明意圖**
   ```asm
   # Load 16 kernels, each with 3 vectors
   DMA.LEN 48  # 16 * 3 = 48
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
  {"index": 0, "word": "0x0002", "dec": 2, "disasm": "CLEAR.P"},
  {"index": 1, "word": "0x00EA", "dec": 234, "disasm": "DMA.LEN 48"},
  {"index": 2, "word": "0x1806", "dec": 6150, "disasm": "DMA.SD 4"}
]
```

### Disassembly 格式 (.disasm)

- **編碼**: 純文字，每行一條指令
- **格式**: `<index>: <disasm>`
- **用途**: 除錯、比對、文件

**範例**:
```
   0: CLEAR.P
   1: DMA.LEN 48
   2: DMA.SD 4
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
| `Undefined label 'XXX'` | J 指令引用未定義的標籤 | 檢查標籤是否存在且拼寫正確 |
| `* out of range` | 操作數超出範圍 | 檢查立即數或暫存器編號範圍 |
| `LOOPEND without previous instruction` | LOOPEND 在檔案開頭或連續使用 | 確保 LOOPEND 前有指令 |
| `J immediate must be even` | J 位址未對齊 | 使用偶數位址或使用標籤 |
| `expects N operands` | 操作數數量錯誤 | 檢查指令語法，補齊或移除多餘操作數 |
| `Cannot open input` | 檔案不存在或無讀取權限 | 檢查檔案路徑和權限 |
| `Undefined template parameter` | Template 中使用未定義參數 | 檢查參數名稱拼寫 |

### 執行錯誤

| 問題 | 可能原因 | 解決方法 |
|------|---------|----------|
| 程式無限迴圈 | LOOPEND 位置錯誤 | 檢查 LOOPEND 是否在正確位置 |
| 記憶體存取錯誤 | DMA.ADDR 超出範圍 | 確認位址在 0-1023 範圍內 |
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
    CLEAR.P              # 清除部分和暫存器

load_data:
    DMA.ADDR 0           # 設定 DMA 起始位址
    DMA.LEN 64           # 載入 64 個資料
    DMA.LD 1             # 載入 double-word, stride=1

compute:
    SETRID.PT 0, 0       # 設定 PID=0, VTID=0

    # MAC 運算
    VMAC p0, vt0         # p0 += vt0 * dmrv
    VMAC p1, vt1         # p1 += vt1 * dmrv
    VMAC p2, vt2         # p2 += vt2 * dmrv
    VMACN p3, vt0        # p3 += vt0 * dmrv, 觸發下一次 DMA

accumulate:
    VPSUM vp0            # 累加到輸出
    CLEAR.P              # 清除暫存器

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
    CLEAR.P

load_kernel:
    DMA.ADDR 0
    DMA.LEN 48           # 16 kernels × 3 vectors = 48
    DMA.SD 4             # 儲存 kernel 資料

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

    DMA.ADDR 0
    DMA.LEN 48
    DMA.LD 4
    SETRID.PT 0, 0

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
    CLEAR.P

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
    CLEAR.P

load_matrix:
    DMA.ADDR 0
    DMA.LEN $(M)         # 使用參數 M
    DMA.SD 4

load_vector:
    DMA.ADDR 0
    DMA.LEN $(N)         # 使用參數 N
    DMA.LHB 1

compute_loop:
    LOOPIN $(M)          # 使用參數 M

    VMULR 1, 1
    VMULRN 1, VTRST

    LOOPEND

accumulate:
    LOOPIN $(BATCH)      # 使用參數 BATCH
    VPSUMR 1
    LOOPEND

    CLEAR.P
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

### 範例 4: 標籤與跳轉

```asm
# Conditional processing with jump

start:
    CLEAR.P
    DMA.ADDR 0
    DMA.LEN 100
    DMA.LD 1

process:
    SETRID.PT 0, 0
    VMAC p0, vt0
    VMACN p1, vt1

check_done:
    # 假設有條件檢查 (實際硬體可能需要其他機制)
    # 這裡示範跳轉用法
    J continue           # 跳轉到 continue 標籤

early_exit:
    HALT

continue:
    VPSUM vp0
    CLEAR.P
    J finish             # 跳轉到 finish

finish:
    HALT
```

**注意**:
- J 指令使用絕對位址
- 標籤會被解析為指令索引 (轉換為 byte address)

---

## 附錄

### A. 指令快速參考

完整指令集請參考:
- **doc/ISA.md**: 簡化指令集摘要
- **doc/Hybridacc PE.md**: 完整指令集與硬體架構文件
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

---

## 聯絡與支援

如有問題或建議，請參考:
- 專案 README.md
- 提交 Issue 或 Pull Request
- 查閱 doc/ 目錄下的詳細文件

**祝您使用愉快！**
