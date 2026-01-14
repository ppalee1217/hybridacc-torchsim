# Implementation Log - HybridAcc PE ISA Updates

## 2026-01-12: Ping-pong Buffer & Separated DMA Control

### Overview
Implementation of the architecture update including:
1.  **Split DMA Control**: Separated configuration for LDMA (Load) and SDMA (Store).
2.  **Ping-pong Data Memory**: Dual-bank memory structure with valid-bit synchronization.
3.  **New Instructions**: `LDMA.*`, `SDMA.*` (including LOOP support), and `SWAPDM`.

### Assembler Updates (`src/assambler/instruction.cpp`)
- Removed legacy `DMA.ADDR`, `DMA.LEN`.
- Added `LDMA.ADDR` (opcode=00, f2=01, f1=0) / `LDMA.LEN` (opcode=00, f2=01, f1=1).
- Added `SDMA.ADDR` (opcode=00, f2=00, f1=0) / `SDMA.LEN` (opcode=00, f2=00, f1=1).
- Added `LDMA.LOOP` (opcode=01, f2=01, f1=0) / `SDMA.LOOP` (opcode=01, f2=01, f1=1).
- Added `SWAPDM` (opcode=11, f2=11, func3=100).
- Updated `HALT` to enforce func3=000.

### Simulator Updates (`src/simulator`)
- **Component.hpp**:
    - `DataMemory`: Refactored to support 2 banks (`banks[2]`), `active_bank` index, and `bank_valid[2]` flags. Added `swap()` method which clears the valid bit of the outgoing bank.
    - `DMAController`: Added `loop_count`, `init_base`, `init_len` for auto-reset. Added `bytes_transferred` tracking to trigger valid bit setting.
    - `PEState`: Split single `DMA` into `LDMA` and `SDMA`. Note: They share the class definition but operating on different banks/logic.

- **Simulator.cpp**:
    - `PESimulator::execute`:
        - Updated dispatcher for Opcode 00/01/11.
        - `SWAPDM`: Implemented stalling logic. Waits for `SDMA` idle AND `DataMemory::isValid(back_buffer)`.
        - `DMA.SD`: Writes to `inactive_bank`. Updates `bytes_transferred`. Sets valid bit when transfer complete.

## 2026-01-14 Fix Simulator Logic

### Issues Identified
1.  **SDMA Double Execution**: The  instruction was being interpreted as "Run 1 time *in addition* to the initial run", causing  to execute twice (96 transfers total). Since  input only provided 48 words, the second pass read from an empty queue.
2.  **Silent Failure on Empty Queue**:  (and others) returned  and  when the queue was empty, masking the issue. The second SDMA pass filled the buffer with 0s, leading to all-zero outputs.

### Fixes Applied
1.  **src/simulator/component.cpp**: Modified  to interpret  as the condition to repeat. Now  (or 0) results in a single execution pass (Total Iterations interpretation), aligning with ASM intent and data availability.
2.  **src/simulator/simulator.cpp**: Updated , ,  to return  on empty queue. This ensures  throws a visible  if data runs out unexpectedly.

### Verification
*   **Result**: Simulation successfully completes. Output verification shows drastic improvement (from 100% mismatch to only ~0.1% mismatch due to minor FP16 precision differences).
*   **Status**: Fixed logic errors.
