# Hybridacc PE and Instruction Set Architecture (ISA) Documentation

## Overview
This document describes the instruction set architecture (ISA) for a custom processor. Each instruction is represented by a 16-bit binary encoding, with specific fields defining the operation, operands, and additional parameters.


---
## PE Components
- SRAM
    - Data Memmory (DM), 16-bit, 256 words
    - Instruction Memory (IM), 16-bit, 256 words
- ALU (fp16 *4, int8 *4, int4 *16)
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

### Bit-fielding
Each instruction is encoded in a 16-bit format, with specific fields for:
- loop-end: inst[0]
- opcode: inst[2:1]
- function code: inst[4:3]
- additional parameters
- func3: inst[15:13]
- func1: inst[12]

### 1. **Data Movement Instructions**
These instructions handle data transfer between memory and registers.
- **DMA.ADDR, start_addr**: Load the starting address for DMA operations.
    - Opcode: `00`, Function code: `01`, func1 = `0`
    - Fields: `start_addr[9:7]` (inst[15:13]),`start_addr[0|6:1]` (inst[11:5])
    - Notes: Sets the DMA starting address.

- **DMA.LEN, len**: Load the length for DMA operations.
    - Opcode: `00`, Function code: `01`, func1 = `1`
    - Fields: `len[9:7]` (inst[15:13]),`len[0|6:1]` (inst[11:5])
    - Notes: Specifies the length of the DMA transfer.

- **DMA.LB, stride**: Load a byte to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `000`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a byte from memory to DMRV.

- **DMA.LH, stride**: Load a half-word (16bits) to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `001`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a half-word from memory to DMRV.

- **DMA.LW, stride**: Load a word (32bits) to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `010`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a word from memory to DMRV.

- **DMA.LD, stride**: Load a double-word (64bits) to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `011`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a double-word from memory to DMRV.

- **DMA.LBB, stride**: Load a byte and broadcast to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `100`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a byte and broadcasts it to DMRV.

- **DMA.LHB, stride**: Load a half-word and broadcast to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `101`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a half-word and broadcasts it to DMRV.

- **DMA.LWB, stride**: Load a word and broadcast to DMRV (nonblocking).
    - Opcode: `00`, Function code: `10`, func3: `110`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a word and broadcasts it to DMRV.

- **DMA.SD, stride**: Store a double-word from PS port (blocking).
    - Opcode: `00`, Function code: `11`, func3: `011`
    - Fields: `stride` (inst[12:10])
    - Notes: Transfers a double-word from PS port.

- **TSTORE, trd**: Store data from PD port.
    - Opcode: `01`, Function code: `00`, func3: `000`
    - Fields: `trd` (inst[7:5])
    - Notes: Transfers data from PD port.

- **TSHIFT, kernel_size**: Kernel size shift operation.
    - Opcode: `01`, Function code: `00`, func3: `001`
    - Fields: `kernel_size[2:0]` (inst[12:10])
    - Notes: Performs kernel size shift.
        (K3: `000`, K5: `001`, K7: `010`)

---

### 2. **Arithmetic Instructions**
These instructions perform mathematical operations.

- **VMAC, prd, vtrs**: Multiply-accumulate with fixed DMA out.
    - Opcode: `10`, Function code: `01`, func3: `000`, func1: `0`
    - Fields: `prd` (inst[9:5]), `vtrs` (inst[11:10])
    - Notes: Performs MAC operation and outputs fixed DMRV.

- **VMACN, prd, vtrs**: Multiply-accumulate and trigger next DMA out.
    - Opcode: `10`, Function code: `01`, func3: `000`, func1: `1`
    - Fields: `prd` (inst[9:5]) , `vtrs` (inst[11:10])
    - Notes: Performs MAC operation and triggers next DMA out.

- **VMACR, pstride, vtstride**: Multiply-accumulate with register control.
    - Opcode: `10`, Function code: `01`, func3: `001`, func1: `0`
    - Fields: `pstride` (inst[9:5]), `vtstride` (inst[11:10])
    - Notes: Resets `pid` if `pstride == 31`, resets `vtid` if `vtstride == 3`.

- **VMACRN, pstride, vtstride**: Multiply-accumulate with register control and next DMA.
    - Opcode: `10`, Function code: `01`, func3: `001`, func1: `1`
    - Fields: `pstride` (inst[9:5]), `vtstride` (inst[11:10])
    - Notes: Resets `pid` if `pstride == 31`, resets `vtid` if `vtstride == 3`, triggers next DMA.

- **VMUL, vprd, vtrs**: Multiply with fixed DMA out.
    - Opcode: `10`, Function code: `01`, func3: `010`, func1: `0`
    - Fields: `vprd` (inst[9:5]), `vtrs` (inst[11:10])
    - Notes: Performs multiplication and outputs fixed DMA.

- **VMULN, vprd, vtrs**: Multiply and trigger next DMA out.
    - Opcode: `10`, Function code: `01`, func3: `010`, func1: `1`
    - Fields: `vprd` (inst[9:5]), `vtrs` (inst[11:10])
    - Notes: Performs multiplication and triggers next DMA.

- **VMULR, vpstride, vtstride**: Multiply with register control.
    - Opcode: `10`, Function code: `01`, func3: `011`, func1: `0`
    - Fields: `vpstride` (inst[9:5]), `vtstride` (inst[11:10])
    - Notes: Resets `pid` if `pstride == 31`, resets `vtid` if `vtstride == 3`.

- **VMULRN, vpstride, vtstride**: Multiply with register control and next DMA.
    - Opcode: `10`, Function code: `10`, func3: `011`, func1: `1`
    - Fields: `vpstride` (inst[9:5]), `vtstride` (inst[11:10])
    - Notes: Resets `pid` if `pstride == 31`, resets `vtid` if `vtstride == 3`, triggers next DMA.

- **VPSUM, vprs**: Vector sum operation.
    - Opcode: `10`, Function code: `01`, func3: `100`
    - Fields: `vprs` (inst[9:5])
    - Notes: Computes `PLO = PLI + psum[vprs]`.

- **VPSUMR, vpstride**: Vector sum with stride update.
    - Opcode: `10`, Function code: `01`, func3: `101`
    - Fields: `vpstride` (inst[9:5])
    - Notes: Computes `PLO = PLI + psum[vpidx]`, updates `vpidx += vpstride`.

---

### 3. **Control Flow Instructions**
These instructions manage program flow and loops.

- **J, imm**: Jump to a specific address.
    - Opcode: `01`, Function code: `10`
    - Fields: `imm[9:7|10|0|6:1]` (inst[15:5])
    - Notes: Performs an unconditional jump.

- **LOOPIN, loop_count**: Push loop count and store next PC.
    - Opcode: `01`, Function code: `11`, func1: `0`
    - Fields: `loop_count[9:7]` (inst[15:13]), `loop_count[0|6:1]` (inst[11:5])
    - Notes: Initializes a loop and stores the next program counter.)

- **LOOPBREAK**: Pop loop count.
    - Opcode: `01`, Function code: `11`, func1: `1`
    - Notes: Exits the current loop.

- **LOOPEND**: Virtual assembly code for loop termination.
    - Notes: Marks the end of a loop.
    - Set previous machine code's LSB to 1 to indicate loop end.

---

### 4. **System-Level Instructions**
These instructions handle system-level operations.

- **NOP**: No operation. ()
    - Opcode: `10`, Function code: `00`
    - Notes: Performs no operation.

- **SETRID.PT, pid, vtid**: Set resource ID for PID and VTID.
    - Opcode: `10`, Function code: `10`, func3: `011`
    - Fields: `pid` (inst[9:5]), `vtid` (inst[11:10])
    - Notes: Sets resource IDs for PID and VTID.

- **SETRID.P, pid**: Set resource ID for PID.
    - Opcode: `10`, Function code: `10`, func3: `001`
    - Fields: `pid` (inst[9:5])
    - Notes: Sets resource ID for PID.

- **SETRID.T, vtid**: Set resource ID for VTID.
    - Opcode: `10`, Function code: `10`, func3: `010`
    - Fields: `vtid` (inst[11:10])
    - Notes: Sets resource ID for VTID.

- **CLEAR.T**: Clear VTID.
    - Opcode: `10`, Function code: `11`, func3: `000`
    - Notes: Clears VTID.

- **CLEAR.P**: Clear PID.
    - Opcode: `10`, Function code: `11`, func3: `001`
    - Notes: Clears PID.

- **HALT**: Halt the processor.
    - Opcode: `11`, Function code: `11`
    - Notes: Stops processor execution.

---

## Notes
- Each instruction is encoded in 16 bits.
- Fields are used to specify operands and parameters.
- Some instructions include reset conditions for specific fields.
