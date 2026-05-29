# Hybridacc PE and Instruction Set Architecture (ISA) Documentation

文件樹： [../../../doc/index.md](../../../doc/index.md) -> [../README.md](../README.md) -> 本頁。

## Overview
This document describes the instruction set architecture (ISA) for the HybridAcc PE. Each instruction is 16 bits and uses fixed bit-fields to encode the operation, operands, and parameters.

## Recent Revisions (2026-02)
1. ISA v3 bit-frame: payload[15:6], func1[5], func2[4:3], opcode[2:1], LE[0].
2. SYSCTRL/SYS.SYNC flag-based system control and SWAPDM sync.
3. DMA uses Static/Active registers; ACT flag triggers copy+start.


---
## PE Components
- SRAM
    - Data Memmory (DM), 16-bit, 256 words (Ping-pong buffer, LDMA sees one bank, SDMA manages Mux)
    - Instruction Memory (IM), 16-bit, 256 words
- ALU (fp16 *4)
    - VMAC Unit
    - VMUL Unit
    - VPSUM Unit
- PC
    - Program Counter
- LOOP stack
    - Loop stack for managing loop counts and pc addresses
- RF
    - T/VT (Transform)
        - 16-bit, 12 registers (T0–T11, VT0–VT2)
    - P/VP (Partial Sum)
        - 16-bit, 32 registers (P0–P31, VP0–VP7)
        - 64-bit, 24 vector registers (VP8–VP31)
    - DMRV (Data Memory Read out Vector, virtual register)
        - 64-bit, vector mapping registers
- CU
    - Control Unit
- I/O
    - Local Input/Output interfaces for external communication (PLO/PLI ,partial sum), 64-bit data width
    - Dynamic port (PD, input activations), 16-bit data width
    - Static port (PS, instructions / weights), 64-bit data width

---
## Instruction Categories

### Bit-fielding (ISA v3)
Each instruction is encoded in a 16-bit format, with specific fields for:
- loop-end: inst[0] (LOOPEND marker)
- opcode: inst[2:1]
- func2: inst[4:3]
- func1: inst[5]
- payload: inst[15:6] (10 bits; some ops reuse payload[8:6] as func3)

#### Bit-field Overview
```
15        6 5 4  3 2  1 0
[ payload ][f1][f2][opcode][LE]
```

### 1. **Data Movement Instructions (opcode=00)**
These instructions handle data transfer between memory and registers.
- **LDMA.ADDR, start_addr**: Load the starting address for LDMA operations.
    - Opcode: `00`, func2: `00`, func1: `0`
    - Fields: `payload = start_addr[9:0]`
    - Notes: Sets the LDMA starting address.

- **LDMA.LEN, len**: Load the length for LDMA operations.
    - Opcode: `00`, func2: `01`, func1: `0`
    - Fields: `payload = len[9:0]`
    - Notes: Specifies the length of the LDMA transfer.

- **LDMA.LOOP, count**: Set loop count for LDMA auto-reset.
    - Opcode: `00`, func2: `10`, func1: `0`
    - Fields: `payload = loop_count[9:0]`
    - Notes: Sets loop count for LDMA.

- **SDMA.ADDR, start_addr**: Load the starting address for SDMA operations.
    - Opcode: `00`, func2: `00`, func1: `1`
    - Fields: `payload = start_addr[9:0]`
    - Notes: Sets the SDMA starting address.

- **SDMA.LEN, len**: Load the length for SDMA operations.
    - Opcode: `00`, func2: `01`, func1: `1`
    - Fields: `payload = len[9:0]`
    - Notes: Specifies the length of the SDMA transfer.

- **SDMA.LOOP, count**: Set loop count for SDMA auto-reset.
    - Opcode: `00`, func2: `10`, func1: `1`
    - Fields: `payload = loop_count[9:0]`
    - Notes: Sets loop count for SDMA.

- **LDMA.LB/LH/LW/LD/LBB/LHB/LWB, stride**: Load to DMRV (nonblocking, LDMA).
    - Opcode: `00`, func2: `11`, func1: `0`
    - Fields: `payload[11:9]=stride`, `payload[8:6]=func3`
    - Notes: Transfers a byte from memory to DMRV.

- **SDMA.SD, stride**: Store a double-word from PS port (blocking, SDMA).
    - Opcode: `00`, func2: `11`, func1: `0`, func3: `111`
    - Fields: `payload[11:9]=stride`
    - Notes: Transfers a double-word from PS port using SDMA.

- **TSTORE, trd**: Store data from PD port.
    - Opcode: `00`, func2: `11`, func1: `1`, func3: `000`
    - Fields: `payload[15:12]=trd`
    - Notes: Transfers data from PD port.

- **VTSTORE, vtrd**: Store vector data from PD port.
    - Opcode: `00`, func2: `11`, func1: `1`, func3: `001`
    - Fields: `payload[10:9]=vtrd`
    - Notes: Loads 4 lanes into VT registers.

- **TSHIFT, kernel_size**: Kernel size shift operation.
    - Opcode: `00`, func2: `11`, func1: `1`, func3: `010`
    - Fields: `payload[11:9]=kernel_size (3/5/7)`
    - Notes: Performs kernel size shift.
        (K3/K5/K7 直接填入數值 3/5/7)

---

### 2. **Arithmetic Instructions (opcode=01)**
These instructions perform mathematical operations.

- **VMAC, prd, vtrs**: Multiply-accumulate with fixed DMA out.
    - Opcode: `01`, func2: `00`, func3: `000`, func1: `0`
    - Fields: `payload[15:11]=prd`, `payload[10:9]=vtrs`
    - Notes: Performs MAC operation and outputs fixed DMRV.

- **VMACN, prd, vtrs**: Multiply-accumulate and trigger next DMA out.
    - Opcode: `01`, func2: `00`, func3: `000`, func1: `1`
    - Fields: `payload[15:11]=prd`, `payload[10:9]=vtrs`
    - Notes: Performs MAC operation and triggers next DMA out.

- **VMACR, pstride, vtstride**: Multiply-accumulate with register control.
    - Opcode: `01`, func2: `00`, func3: `001`, func1: `0`
    - Fields: `payload[15:11]=pstride`, `payload[10:9]=vtstride`
    - Notes: Stride control with reset conditions.

- **VMACRN, pstride, vtstride**: Multiply-accumulate with register control and next DMA.
    - Opcode: `01`, func2: `00`, func3: `001`, func1: `1`
    - Fields: `payload[15:11]=pstride`, `payload[10:9]=vtstride`
    - Notes: Stride control with reset conditions, triggers next DMA.

- **VMUL, vprd, vtrs**: Multiply with fixed DMA out.
    - Opcode: `01`, func2: `01`, func3: `000`, func1: `0`
    - Fields: `payload[15:11]=vprd`, `payload[10:9]=vtrs`
    - Notes: Performs multiplication and outputs fixed DMA.

- **VMULN, vprd, vtrs**: Multiply and trigger next DMA out.
    - Opcode: `01`, func2: `01`, func3: `000`, func1: `1`
    - Fields: `payload[15:11]=vprd`, `payload[10:9]=vtrs`
    - Notes: Performs multiplication and triggers next DMA.

- **VMULR, vpstride, vtstride**: Multiply with register control.
    - Opcode: `01`, func2: `01`, func3: `001`, func1: `0`
    - Fields: `payload[15:11]=vpstride`, `payload[10:9]=vtstride`
    - Notes: Multiply with stride control.

- **VMULRN, vpstride, vtstride**: Multiply with register control and next DMA.
    - Opcode: `01`, func2: `01`, func3: `001`, func1: `1`
    - Fields: `payload[15:11]=vpstride`, `payload[10:9]=vtstride`
    - Notes: Multiply with stride control, triggers next DMA.

- **VPSUM, vprs**: Vector sum operation.
    - Opcode: `01`, func2: `10`, func3: `000`
    - Fields: `payload[15:11]=vprs`
    - Notes: Computes `PLO = PLI + psum[vprs]`.

- **VPSUMR, vpstride**: Vector sum with stride update.
    - Opcode: `01`, func2: `10`, func3: `001`
    - Fields: `payload[15:11]=vpstride`
    - Notes: Computes `PLO = PLI + psum[vpidx]`, updates `vpidx += vpstride`.

---

### 3. **Control Flow Instructions (opcode=10)**
These instructions manage program flow and loops.

- **LOOPIN, loop_count**: Push loop count and store next PC.
    - Opcode: `10`, func2: `00`, func1: `0`
    - Fields: `payload = loop_count[9:0]`
    - Notes: Initializes a loop and stores the next program counter.)

- **LOOPBREAK**: Pop loop count.
    - Opcode: `10`, func2: `00`, func1: `1`
    - Notes: Exits the current loop.

- **LOOPEND**: Virtual assembly code for loop termination.
    - Notes: Marks the end of a loop by setting bit0 of the previous word.

---

### 4. **System-Level Instructions (opcode=10)**

        - Fields: `payload = (len-1)[9:0]`
    - Opcode: `10`, func2: `10`, func1: `0`
    - Notes: Performs no operation.
- **SYS.CTRL (flags)**: System control flags.
        - Fields: `payload = (len-1)[9:0]`
    - Fields (payload bits):
        - bit7 `SDMA.ACT`, bit6 `SDMA.RST`, bit5 `LDMA.ACT`, bit4 `LDMA.RST`
    - Notes: ACT 會將 Static Registers 複製到 Active 並啟動 DMA。
        - Fields: `payload = (loop_count-1)[9:0]`
- **SYS.SYNC (SWAPDM)**: System sync / DM swap.
    - Opcode: `10`, func2: `01`, func1: `1`
    - Notes: 等待 SDMA 完成後交換 DM bank。
        - Fields: `payload = (loop_count-1)[9:0]`
- **HALT**: Halt the processor.
    - Opcode: `10`, func2: `11`, func1: `0`

        - Fields: `payload[10:9]=kernel_size_code (K3/K5/K7 -> 0/1/2)`

                (assembler accepts K3/K5/K7 or 3/5/7; encoded as 0/1/2)
- pstride == 31 resets pid
- vpstride == 31 resets vpidx

        - Fields: `payload = (loop_count-1)[9:0]`
- LOOPEND only sets bit0 of the previous instruction; it does not affect other fields.

- Each instruction is encoded in 16 bits.
        - Notes: Exits the current loop (implemented in simulator decode path; assembler mnemonic pending).
- ISA v3 移除 J；SETRID/CLEAR 指令改為 SYS.CTRL 旗標別名。

 - LEN/LOOP/LOOPIN 類計數欄位在機器碼層皆採 `N-1` 編碼。
