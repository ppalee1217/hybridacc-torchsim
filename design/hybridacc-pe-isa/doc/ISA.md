# Hybridacc PE and Instruction Set Architecture (ISA) Documentation

## Overview
本文件描述 HybridAcc PE 16-bit 指令集 (ISA)。所有指令 16 bits：
- bit0 : LOOPEND 標記 (由偽指令 LOOPEND 設定，不影響主體解碼)
- bits[2:1] : opcode
- bits[4:3] : funct2
- bit12 : func1 (子功能 / 區分類型)
- bits[15:13] : func3 (主功能或指令子類)
- bits[11:5] : payload/立即數/操作數片段

## 近期修訂 (2025-08)
1. DMA.ADDR / DMA.LEN 重新編碼為 10-bit 資料，統一 opcode=00 funct2=01，以 func1=0/1 區分。
2. VMULR / VMULRN 與 VMAC* 同步使用 funct2=01。
3. TSTORE / TSHIFT 定義為 opcode=01 funct2=00，func3=000/001。
4. (2026-01) Ping-pong DM 架構更新: LDMA/SDMA 分離設定 (opcode=00 f2=00/01)，新增 LOOP 重置 (opcode=01 f2=01)，新增 SWAPDM (opcode=11 f2=11 func3=100)。

## 位元欄位總覽
```
15   13 12 11    10  5 4  3 2  1 0
[func3][f1][b11][  payload  ][f2][opcode][LE]
```
LE = loop-end (僅由偽指令設定)
注意：payload 依指令類型再拆分。

### 共用 10-bit 壓縮格式 (DMA.ADDR / DMA.LEN / LOOPIN / J 內部類似方式)
對 10 或 11-bit 值採樣：value[9:7] -> bits[15:13], value[0] -> bit11, value[6:1] -> bits[10:5]，bit12 為 func1 (區分 ADDR/LEN 或其他)。J/LOOPIN 額外插入 bit10 到 bit12 (func1) 時使用。

## Data Movement
| 指令 | 編碼重點 | 說明 |
|------|----------|------|
| LDMA.ADDR / LEN | opcode=00 f2=01 f1=0/1; value 10-bit | LDMA 設定 (Addr/Len) |
| SDMA.ADDR / LEN | opcode=00 f2=00 f1=0/1; value 10-bit | SDMA 設定 (Addr/Len) |
| LDMA.LOOP | opcode=01 f2=01 f1=0; value 10-bit | LDMA Loop 自動重置設定 |
| SDMA.LOOP | opcode=01 f2=01 f1=1; value 10-bit | SDMA Loop 自動重置設定 |
| LDMA.L* stride | opcode=00 f2=10 func3 區分型別 | LDMA 載入 / 廣播 (至 DMRV) |
| SDMA.SD stride | opcode=00 f2=11 func3=011 | SDMA 寫回 (自 PS Port) |
| TSTORE trd | opcode=01 f2=00 func3=000 trd→[7:5] | 輸入暫存儲存 |
| TSHIFT k | opcode=01 f2=00 func3=001 kcode→[12:10] | Kernel shift |

## Arithmetic
統一 opcode=10 funct2=01。
| 指令 | func3 | func1 | 操作數 | 說明 |
|------|-------|-------|--------|------|
| VMAC / VMACN | 000 | 0/1 | prd[9:5], vtrs[11:10] | MAC + optional next DMA |
| VMACR / VMACRN | 001 | 0/1 | pstride, vtstride | stride 控制 + reset 條件 |
| VMUL / VMULN | 010 | 0/1 | prd, vtrs | 乘法 |
| VMULR / VMULRN | 011 | 0/1 | vpstride, vtstride | 乘法 + stride 控制 |
| VPSUM | 100 | 0 | vprs[9:5] | 部分和加到輸出 |
| VPSUMR | 101 | 0 | vpstride[9:5] | 加 + stride 增量 |

## Control
| 指令 | 編碼 | 說明 |
|------|------|------|
| J imm/label | opcode=01 f2=10 11-bit 重排 (9:7|10|0|6:1) | 絕對位址跳轉 (2-byte 對齊) |
| LOOPIN count | opcode=01 f2=11 f1=0 壓縮 10-bit | 推入 loop 次數 |
| LOOPBREAK | opcode=01 f2=11 f1=1 | 結束 loop |
| LOOPEND | (偽) 將前一 word bit0=1 | 標註 loop 結尾 |

## System
| 指令 | 編碼 | 說明 |
|------|------|------|
| NOP | opcode=10 f2=00 | 無動作 |
| SWAPDM | opcode=11 f2=11 func3=100 | 交換 DM Bank (Wait SDMA) |
| SETRID.P / T / PT | opcode=10 f2=10 func3=001/010/011 | 設定資源 ID |
| CLEAR.T / CLEAR.P | opcode=10 f2=11 func3=000/001 | 清除 ID |
| HALT | opcode=11 f2=11 func3=000 | 停止 |

## C 語言解碼輔助巨集 (ha_format.h)
提供以下：
- 取得欄位：HA_GET_OPCODE(w), HA_GET_FUNCT2, HA_GET_FUNC1, HA_GET_FUNC3, HA_GET_PAYLOAD, HA_GET_LOOP_END
- 組建：ha_make_instr(func3, func1, payload, funct2, opcode, loop_end)
- 設定欄位：HA_SET_PAYLOAD, HA_SET_FUNC1, HA_SET_FUNC3, HA_SET_LOOP_END
- 文字格式化：ha_format_instr

### 解碼 LDMA.ADDR / LDMA.LEN 10-bit 值範例 (C):
```c
uint16_t w; // 已讀取
if(HA_GET_OPCODE(w)==0 && HA_GET_FUNCT2(w)==1){
    int func1 = HA_GET_FUNC1(w);
    int func3 = HA_GET_FUNC3(w);
    int payload = HA_GET_PAYLOAD(w); // bits[11:5]
    int bits6_1 = payload & 0x3F;      // [10:5]
    int bit0 = (payload >> 6) & 0x1;   // [11]
    int value = (func3 << 7) | (bits6_1 << 1) | bit0; // 10-bit
    if(func1==0){ /* LDMA.ADDR value */ } else { /* LDMA.LEN value */ }
}
```

### 組出 LDMA.LEN 例子：
```c
uint16_t w = 0;
int val = 0x155; // 10-bit
w |= (0 /*opcode*/)<<1;
w |= (1 /*funct2*/)<<3;
w |= ((val>>7)&0x7) << 13; // func3
w |= 1 << 12;              // func1=1 -> LEN
w |= (val & 1) << 11;
w |= ((val>>1)&0x3F) << 5;
```

## Reset 條件
- pstride == 31 重設 pid
- vpstride == 31 重設 vpidx
- vtstride == 3 重設 vtid

## Loop 標記
- LOOPEND 只設定前一指令 bit0，不影響其他欄位；反組譯時加註 `; LOOPEND`。

## 可能改進
- 進一步正式化 JSON schema
- 增加 ELF / DWARF 支援
- 擴充模擬器狀態轉儲介面

## 版本追蹤
此檔案與 README.md 一致後 commit：更新 DMA.ADDR / DMA.LEN 編碼。
