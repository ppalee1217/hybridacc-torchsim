# RV32I Instruction Set Architecture (ISA)

The RV32I ISA is the base integer instruction set for RISC-V, designed for 32-bit processors. It provides a minimal set of instructions required for general-purpose computing.

## Instruction Categories

### 0. pseudo Instructions
- **NOP**: No operation. This is a pseudo-instruction that translates to `ADDI x0, x0, 0`.
- **LI**: Load immediate. This pseudo-instruction loads a 32-bit immediate value into a register, which may require multiple instructions.
- **MV**: Move. This pseudo-instruction copies the value from one register to another, translating to `ADDI rd, rs1, 0`.
- **J**: Jump. This pseudo-instruction jumps to a target address, translating to `JAL x0, offset`.
- **RET**: Return from a function. This pseudo-instruction translates to `JALR x0, x1, 0`.
- **CALL**: Call a function. This pseudo-instruction translates to `JAL x1, offset`.
-

### 1. Arithmetic Instructions
- **ADD**: Add two registers. `ADD rd, rs1, rs2` computes `rd = rs1 + rs2`.
- **SUB**: Subtract two registers. `SUB rd, rs1, rs2` computes `rd = rs1 - rs2`.
- **MUL**: Multiply two registers (if M extension is supported). `MUL rd, rs1, rs2` computes `rd = rs1 * rs2`.

### 2. Logical Instructions
- **AND**: Bitwise AND of two registers. `AND rd, rs1, rs2` computes `rd = rs1 & rs2`.
- **OR**: Bitwise OR of two registers. `OR rd, rs1, rs2` computes `rd = rs1 | rs2`.
- **XOR**: Bitwise XOR of two registers. `XOR rd, rs1, rs2` computes `rd = rs1 ^ rs2`.

### 2.1. Comparison Instructions
- **SLT**: Set less than. `SLT rd, rs1, rs2` sets `rd = 1` if `rs1 < rs2`, otherwise `rd = 0`.
- **SLTU**: Set less than unsigned. `SLTU rd, rs1, rs2` sets `rd = 1` if `rs1 < rs2` (unsigned comparison), otherwise `rd = 0`.

### 3. Immediate Instructions
- **ADDI**: Add immediate value to a register. `ADDI rd, rs1, imm` computes `rd = rs1 + imm`.
- **MULI**: Multiply immediate value with a register (if M extension is supported). `MULI rd, rs1, imm` computes `rd = rs1 * imm`.
- **LUI**: Load upper immediate. `LUI rd, imm` loads the immediate value into the upper 20 bits of `rd`.

### 4. Load and Store Instructions
- **LDV**: Load vector. `LDV vrd, offset(rs1)` loads a vector from memory into register `rd`.
- **STV**: Store vector. `STV vrs2, offset(rs1)` stores a vector from register `rs2` into memory.

### 5. Control Flow Instructions
- **LOOP**: Loop instruction. `LOOP rd, rs1, offset` decrements `rd` and jumps to `offset` if `rd` is not zero.
- **JMP**: Jump instruction. `JMP offset` unconditionally jumps to the specified offset.
- **BEQ**: Branch if equal. `BEQ rs1, rs2, offset` branches to `offset` if `rs1` equals `rs2`.

## Registers
32 general-purpose registers (`x0` to `x31`), where `x0` is hardwired to zero.
2 vector registers (`v0` to `v1`) for vector operations.

## Encoding
Instructions are encoded in a 32-bit format, with specific fields for opcode, source and destination registers, immediate values, and function codes.
## Example Instruction Formats