# HybridAcc ISA 工具套件

本目錄提供 HybridAcc PE 16-bit ISA 的：
- C++ 組譯器 ha-asm
- C++ 反組譯器 ha-objdump
- 靜態函式庫 (hybridacc_asm) 內含 Assembler / Disassembler 類別
- 基礎單元測試 (tests/test_assembler.cpp)

參考文件：doc/Hybridacc PE.md （快速概要：doc/ISA.md）

## 建置
```
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
cmake --build . -j
ctest --output-on-failure  # 執行測試 (可選)
```
產生的執行檔位於 build/src/assambler/：
- ha-asm
- ha-objdump

可使用 install：
```
cmake --install . --prefix <prefix>
```

## ha-asm 使用方式
```
ha-asm <input.asm> [-o output.bin] [--hex output.hex] [--json out.json|-]
```
選項說明：
- -o output.bin : 以 little-endian 寫出 16-bit 機器碼串流
- --hex output.hex : 每行一個 0xXXXX
- --json out.json : 產出 JSON 陣列；使用 - 代表輸出到 stdout
- 若未指定任何輸出，預設輸出 .hex 檔

JSON 格式範例：
```json
[
  {"index":0, "word":"0x0002", "dec":2, "disasm":"NOP"},
  {"index":1, "word":"0xE00E", "dec":57358, "disasm":"HALT"}
]
```
欄位：
- index : 指令序號 (以 16-bit word 為單位)
- word  : 16-bit 機器碼 (十六進位字串)
- dec   : 十進位整數值
- disasm: 反組譯字串 (若 bit0=1 會附加 ; LOOPEND)

## ha-objdump 使用方式
```
ha-objdump <input.bin|input.hex>
```
- .bin : 逐 16-bit 讀取
- 其他：視為 hex 列表 (可含 0x 前綴)
輸出格式：
```
<index>: <disasm>
```

## 組語特性
- 大小寫不敏感 (mnemonic/寄存器)
- 標籤：以 `label:` 宣告，僅支援 J 指令 (絕對目標 = 指令 index)
- 偽指令 LOOPEND：將前一條指令 bit0 設為 1
- 特殊 Token：
  - VTRST -> 對應 vtrs / vtstride = 3 (2-bit 最大值，用於 reset)
  - K3 / K5 / K7 -> TSHIFT kernel size

## 指令摘要 (依最新調整)
(完整欄位請参照 doc/Hybridacc PE.md) 重要差異：
- DMA.ADDR / DMA.LEN 皆為 10-bit 參數，opcode=00 funct2=01，以 func1=0/1 區分。
  - 編碼：value[9:7]→bits[15:13]、value[0]→bit11、value[6:1]→bits[10:5]、func1=0/1。
- TSTORE / TSHIFT : opcode=01 funct2=00，func3=000 / 001
- 算術 VMAC/VMUL/… 全部 funct2=01，VMULR 與 VMULRN 亦使用 funct2=01
- vtstride 目前硬體欄位僅 2 bits (0..3)，reset 條件使用 vtstride==3
- pstride / vpstride 使用 5 bits (0..31)，reset 條件使用 =31

| 類別 | 指令 | 說明 |
|------|------|------|
| Data | DMA.ADDR start_addr | 10-bit start_addr 編碼 value[9:7]|[0]|[6:1] -> bits[15:13|11|10:5] func1=0 |
| Data | DMA.LEN len | 10-bit len 編碼 同上 func1=1 |
| Data | DMA.L[ B/H/W/D/BB/HB/WB ] stride | opcode=00 funct2=11 func3 區分型別 stride→[12:10] |
| Data | DMA.SD stride | opcode=00 funct2=11 func3=011 stride→[12:10] |
| Data | TSTORE trd | opcode=01 funct2=00 func3=000 trd→[7:5] |
| Data | TSHIFT K3/K5/K7 | opcode=01 funct2=00 func3=001 code→[12:10] |
| Arith | VMAC / VMACN prd,vtrs | opcode=10 funct2=01 func3=000 func1=0/1 |
| Arith | VMACR / VMACRN pstride,vtstride | func3=001 |
| Arith | VMUL / VMULN vprd,vtrs | func3=010 |
| Arith | VMULR / VMULRN vpstride,vtstride | func3=011 |
| Arith | VPSUM vprs | func3=100 |
| Arith | VPSUMR vpstride | func3=101 |
| Ctrl | J imm/label | immediate 11 bits 重排 (9:7|10|0|6:1) |
| Ctrl | LOOPIN count | opcode=01 funct2=11 func1=0 |
| Ctrl | LOOPBREAK | opcode=01 funct2=11 func1=1 |
| Pseudo | LOOPEND | 設定前一機器碼 bit0 |
| System | NOP | opcode=10 funct2=00 |
| System | CLEAR.T / CLEAR.P | opcode=10 funct2=11 func3=000/001 |
| System | SETRID.P / T / PT | opcode=10 funct2=10 func3=001/010/011 |
| System | HALT | opcode=11 funct2=11 |

## 限制 / TODO
1. J 目標為絕對指令 index，未提供相對位移。
2. DMA.ADDR / DMA.LEN 目前僅支援 10-bit 值 (0..1023)。
3. 未實作更進階排錯或多重輸出格式 (如 ELF)。
4. 反組譯對未知 pattern 以 .word 0xXXXX 顯示。
5. vtstride >3 直接報錯 (與 2-bit 編碼一致)。

## 測試
執行：
```
ctest -R assembler
```
(或直接執行 build/tests/test_assembler)

## 貢獻流程
1. 修改/新增指令先更新 doc/Hybridacc PE.md / doc/ISA.md
2. 調整 instruction.cpp 中 encode / disasm 對應
3. 補測試案例
4. 建議提交 PR 前跑 ctest 全通過

## JSON 介面整合建議
- 工具鏈腳本可用 `ha-asm prog.asm --json -` 直接讀取 stdout JSON，以便後續載入模擬器或可視化。

## 常見錯誤訊息
| 訊息 | 說明 |
|------|------|
| Unknown mnemonic | 指令名稱錯誤或未實作 |
| Duplicate label | 重複標籤定義 |
| Undefined label | J 指向不存在標籤 |
| * out of range | 操作數超過欄位可表達範圍 |
| LOOPEND without previous instruction | 檔案第一行或連續 LOOPEND |

## 授權 / 版權
(依專案主專案授權規範填寫)

---
如需擴充請提出需求或直接提交變更。
