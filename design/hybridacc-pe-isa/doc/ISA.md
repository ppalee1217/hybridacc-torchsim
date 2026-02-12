# Hybridacc PE and Instruction Set Architecture (ISA) Documentation

## Overview
本文件描述 HybridAcc PE 16-bit 指令集 (ISA)。所有指令 16 bits：
- bit0 : LOOPEND 標記 (由偽指令 LOOPEND 設定，不影響主體解碼)
- bits[2:1] : opcode
- bits[4:3] : func2
- bit5 : func1 (子功能 / 區分類型)
- bits[15:6] : payload / func3（依指令拆分）

## 近期修訂 (2026-02)
1. 統一 v3 編碼：payload[15:6]、func1[5]、func2[4:3]、opcode[2:1]、LE[0]。
2. Data Movement payload 以 10-bit 表示 addr/len/loop 或 stride/func3 組合。
3. System 以 SYSCTRL payload 旗標控制 (不再使用 subop 表)。

## 位元欄位總覽
```
15        6 5 4  3 2  1 0
[ payload ][f1][f2][opcode][LE]
```
LE = loop-end (僅由偽指令設定)
注意：payload 依指令類型再拆分；部分指令以 payload[8:6] 當作 func3。

## Data Movement
| 指令 | 編碼重點 | 說明 |
|------|----------|------|
| LDMA.ADDR | opcode=00 func2=00 func1=0; payload=start_addr[9:0] | LDMA 設定 Addr |
| SDMA.ADDR | opcode=00 func2=00 func1=1; payload=start_addr[9:0] | SDMA 設定 Addr |
| LDMA.LEN | opcode=00 func2=01 func1=0; payload=(len-1)[9:0] | LDMA 設定 Len（組譯輸入 1..1024） |
| SDMA.LEN | opcode=00 func2=01 func1=1; payload=(len-1)[9:0] | SDMA 設定 Len（組譯輸入 1..1024） |
| LDMA.LOOP | opcode=00 func2=10 func1=0; payload=(loop_count-1)[9:0] | LDMA Loop 自動重置設定（組譯輸入 1..1024） |
| SDMA.LOOP | opcode=00 func2=10 func1=1; payload=(loop_count-1)[9:0] | SDMA Loop 自動重置設定（組譯輸入 1..1024） |
| LDMA.LB/LH/LW/LD/LBB/LHB/LWB | opcode=00 func2=11 func1=0; payload[15:12]=0, payload[11:9]=stride, payload[8:6]=func3 | LDMA 載入 / 廣播 (至 DMRV) |
| SDMA.SD | opcode=00 func2=11 func1=0; payload[15:12]=0, payload[11:9]=stride, payload[8:6]=111 | SDMA 寫回 (自 PS Port) |
| TSTORE | opcode=00 func2=11 func1=1; payload[15:12]=trd, payload[11:9]=0, payload[8:6]=000 | 輸入暫存儲存 |
| VTSTORE | opcode=00 func2=11 func1=1; payload[15:12]=0, payload[11]=0, payload[10:9]=vtrd, payload[8:6]=001 | 向量暫存儲存 |
| TSHIFT | opcode=00 func2=11 func1=1; payload[15:12]=0, payload[11]=0, payload[10:9]=kernel_size(2-bit), payload[8:6]=010 | Kernel shift (K3/K5/K7 -> 0/1/2) |

## Arithmetic
統一 opcode=01。
| 指令 | func2 | func1 | func3 | payload | 說明 |
|------|-------|-------|-------|---------|------|
| VMAC / VMACN | 00 | 0/1 | 000 | prd[4:0], vtrs[1:0] | MAC + optional next DMA |
| VMACR / VMACRN | 00 | 0/1 | 001 | pstride[4:0], vtstride[1:0] | stride 控制 + reset 條件 |
| VMUL / VMULN | 01 | 0/1 | 000 | vprd[4:0], vtrs[1:0] | 乘法 |
| VMULR / VMULRN | 01 | 0/1 | 001 | vpstride[4:0], vtstride[1:0] | 乘法 + stride 控制 |
| VPSUM | 10 | 0 | 000 | vprs[4:0] | 部分和加到輸出 |
| VPSUMR | 10 | 0 | 001 | vpstride[4:0] | 加 + stride 增量 |
| VPSUM_VTSTORE | 11 | 0 | 000 | vprs[4:0], vtrd[1:0] | VPSUM + VTSTORE 混合控制 |
| VPSUMR_VTSTORE | 11 | 0 | 001 | vpstride[4:0], vtrd[1:0] | VPSUMR + VTSTORE 混合控制 |
| VPSUM_TSHIFT | 11 | 0 | 010 | vprs[4:0], kernel_size[1:0] | VPSUM + TSHIFT 混合控制 |
| VPSUMR_TSHIFT | 11 | 0 | 011 | vpstride[4:0], kernel_size[1:0] | VPSUMR + TSHIFT 混合控制 |

## Control
| 指令 | 編碼 | 說明 |
|------|------|------|
| LOOPIN count | opcode=10 func2=00 func1=0 payload=(loop_count-1)[9:0] | 推入 loop 次數（組譯輸入 1..1024） |
| LOOPBREAK | opcode=10 func2=00 func1=1 | Pop 目前 loop frame（目前由 simulator 執行；組譯器尚未開放 mnemonic） |
| LOOPEND | (偽) 將前一 word bit0=1 | 標註 loop 結尾 |

## System
| 指令 | 編碼 | 說明 |
|------|------|------|
| SYS.CTRL | opcode=10 func2=01 func1=0 payload 旗標 | 系統控制 (payload[15:14]=0, 其餘旗標: SDMA.ACT/SDMA.RST/LDMA.ACT/LDMA.RST/RST.PID/RST.TID/CLEAR.T/CLEAR.P；SWAPDM 為獨立特例，見下) |
| SYS.SYNC | opcode=10 func2=01 func1=1 payload 旗標 | 系統同步 (payload[0]=SWAPDM；其對應實體位元為 inst[6]) |
| NOP | opcode=10 func2=10 func1=0 payload=0 | 無動作 |
| HALT | opcode=10 func2=11 func1=0 payload=0 | 停止 |

## C 語言解碼輔助巨集 (ha_format.h)
提供以下：
- 取得欄位：HA_GET_OPCODE(w), HA_GET_FUNCT2, HA_GET_FUNC1, HA_GET_FUNC3, HA_GET_PAYLOAD, HA_GET_LOOP_END
- 組建：ha_make_instr(func3, func1, payload, funct2, opcode, loop_end)
- 設定欄位：HA_SET_PAYLOAD, HA_SET_FUNC1, HA_SET_FUNC3, HA_SET_LOOP_END
- 文字格式化：ha_format_instr

### 解碼 LDMA.ADDR / LDMA.LEN 10-bit 值範例 (C):
```c
uint16_t w; // 已讀取
if(HA_GET_OPCODE(w)==0 && (HA_GET_FUNCT2(w)==0 || HA_GET_FUNCT2(w)==1)){
    int func1 = HA_GET_FUNC1(w);
    int payload = HA_GET_PAYLOAD(w); // bits[15:6]
    int value = payload & 0x3FF;     // 10-bit
    if(func1==0){ /* LDMA.ADDR / LDMA.LEN value */ }
    else { /* SDMA.ADDR / SDMA.LEN value */ }
}
```

### 組出 LDMA.LEN 例子：
```c
uint16_t w = 0;
int val = 0x155; // 10-bit
w |= (0 /*opcode*/)<<1;
w |= (1 /*func2*/)<<3;
w |= 0 << 5;                // func1=0 -> LDMA
w |= (val & 0x3FF) << 6;    // payload[15:6]
```

## Reset 條件
- pstride == 31 重設 pid
- vpstride == 31 重設 vpidx
- vtstride == 3 重設 vtid

## Loop 標記
- LOOPEND 只設定前一指令 bit0，不影響其他欄位；反組譯時加註 `; LOOPEND`。
- 若前一指令已被標註 LOOPEND，組譯器會插入一條 NOP 並將其 bit0=1。

## 計數欄位編碼規則
- `LDMA.LEN / SDMA.LEN / LDMA.LOOP / SDMA.LOOP / LOOPIN` 在指令 payload 皆採 `N-1` 編碼。
- 例如：組語 `LDMA.LEN 48`，實際 payload 寫入為 `47`；反組譯再顯示回 `48`。

## 系統指令備註
- SYS.CTRL 若只包含 SWAPDM，實際會編碼為 SYS.SYNC (payload=1)。SWAPDM 不能與其他 SYS.CTRL 旗標併用。

## Kernel Size 編碼
- TSHIFT/VPSUM_TSHIFT/VPSUMR_TSHIFT 接受 K3/K5/K7 或 3/5/7；編碼為 0/1/2 存於 kernel_size[1:0]。

## 可能改進
- 進一步正式化 JSON schema
- 增加 ELF / DWARF 支援
- 擴充模擬器狀態轉儲介面

## 版本追蹤
此檔案與 README.md 一致後 commit：更新 DMA.ADDR / DMA.LEN 編碼。
