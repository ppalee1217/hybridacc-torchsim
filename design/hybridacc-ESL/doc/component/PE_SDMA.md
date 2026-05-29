
# PE / SDMA Design Notes (ESL/SystemC)

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

This document specifies the **hardware-level behavior** of `SDMA` (Store DMA) in the PE ESL (SystemC) model, its **synchronization relationship** with the PE pipeline (specifically the `SWAPDM` instruction), and the corresponding **FSM** and **transition rules**.

> Goal: SDMA STORE runs **in parallel** with the PE control flow; only `SWAPDM` introduces a synchronization/stall point.

---

## 1. Interface and Signal Semantics

### 1.1 Configuration Interface (Writable Only in IDLE)

SDMA configuration registers are written by the decoder via the following signals, and **take effect only when SDMA is in `IDLE`**:

- `set_addr + imm`: set `dma_base` (DM write base address)
- `set_len + imm`: set `dma_len_cfg` (number of elements per loop-phase)
- `set_loop + imm`: set `dma_loop_cfg` (number of loop-phases for the whole task)

### 1.2 Start / Synchronization Signals

- `active`: indicates the arrival of an `SDMA.SD` (store) instruction and is treated as a **one-shot task trigger**.
	- When SDMA sees this in `IDLE`, it latches `stride` and the current configuration, then starts a background store task.
- `swap_in`: indicates that `SWAPDM` is currently being executed in the EXE_M stage.
	- SDMA accepts `swap_in` **only after finishing one loop-phase**, then performs a DM bank role swap.

### 1.3 Stream Data Interface

- `ps_data` (VRDIF): the input data stream for SDMA.
	- SDMA asserts `ps_data.ready_out` only in `RUN`.
	- A transfer occurs on `ps_data.valid_in && ps_data.ready_out` (referred to as *fire*), consuming one element and writing it into DM.

### 1.4 DataMemory Bank Roles

The definition of `bank_sel` follows [design/hybridacc-ESL/simulator/include/PE/DataMemory.hpp](../../simulator/include/PE/DataMemory.hpp):

- `bank_sel = 0`: Write -> Bank0, Read -> Bank1
- `bank_sel = 1`: Write -> Bank1, Read -> Bank0

Therefore, SDMA controls which bank it writes by driving `bank_sel`, while the PE reads the other bank (ping-pong buffering).

---

## 2. Required SDMA Behavior (Restated)

The following is a precise, hardware-oriented restatement of the 6 requirements:

1. **`set_addr/set_len/set_loop` are only effective in `IDLE`**
	 - Prevents reconfiguration while a store task is running.

2. **STORE writes directly to DM**
	- SDMA drives `dm_write_*` directly.

3. **STORE runs in parallel with PE control flow; only `SWAPDM` synchronizes**
	 - `SDMA.SD` only triggers the background task; the pipeline does not wait for task completion.
	 - Synchronization happens only when `SWAPDM` needs the phase boundary.

4. **Moving `len` elements is one loop-phase; the task completes when `loop_count` reaches zero**
	 - A phase completes when exactly `len` successful transfers occur.
	 - `loop_count` decrements once per completed phase; the task is complete when it reaches 0.

5. **After each phase, SDMA waits for `SWAPDM`, swaps the DM buffer, then either starts the next phase or returns to IDLE**
	 - Phase done -> wait for `SWAPDM`.
	 - On `SWAPDM`: swap bank roles.
	 - If phases remain: start next phase immediately; otherwise return to `IDLE`.

6. **If `SWAPDM` arrives early, stall EXE_M and propagate backpressure until the phase finishes**
	 - While SDMA is still in the middle of a phase, `SWAPDM` must be stalled.
	 - Once the phase finishes, `SWAPDM` is allowed to proceed and SDMA performs the bank swap behavior described in (5).

Implementation note: EXE_M typically uses `sdma.busy` (or an equivalent predicate) to decide whether to stall `SWAPDM`.

---

## 3. SDMA FSM Design

The SDMA FSM is organized around **(task, phase)** boundaries and **`SWAPDM` synchronization**.

### 3.1 State Definitions

- `IDLE`
	- Accepts configuration writes (`set_addr/set_len/set_loop`).
	- Accepts the `SDMA.SD` trigger (`active`) to start phase 0 of a new task.
	- Optionally allows `SWAPDM` to swap bank roles even while idle (pure role swap).

- `RUN`
	- Background store is active.
	- `ps_data.ready_out = 1`.
	- Each *fire* (`valid && ready`) writes one element to DM and decrements `len_rem`.
	- When `len_rem` reaches 0, the current phase is complete.

- `WAIT_SWAP`
	- The current phase is complete; SDMA stops consuming input (`ps_data.ready_out = 0`).
	- Waits for `swap_in` (i.e., `SWAPDM` execution).
	- On `swap_in`, swaps `bank_sel`, then either starts the next phase (if phases remain) or finishes.

- `FINISH`
	- Optional one-cycle terminal state to pulse `done=1`.
	- Returns to `IDLE` on the next cycle.

### 3.2 Key Registers / Counters

- `dma_base`: DM base address (configured by `set_addr`)
- `dma_len_cfg`: elements per phase (configured by `set_len`)
- `dma_loop_cfg`: number of phases per task (configured by `set_loop`)
	- ESL convention: `loop_cfg == 0` may be normalized to 1 to avoid accidental no-op tasks.
- `dma_stride`: address stride between consecutive writes
- `dma_offset`: current offset from `dma_base`
- `dma_len_rem`: remaining elements in the current phase
- `dma_loops_rem`: remaining phases in the current task

---

## 4. State Transitions (Conditions / Actions)

The rules below are written as “condition → next state / actions”.

### 4.1 IDLE

- `set_addr=1` → `dma_base = imm`
- `set_len=1` → `dma_len_cfg = imm`
- `set_loop=1` → `dma_loop_cfg = imm`

- `active` (SDMA.SD trigger) → `RUN`
	- Actions:
		- latch `dma_stride = stride`
		- `dma_offset = 0`
		- `dma_len_rem = dma_len_cfg`
		- `dma_loops_rem = normalize(dma_loop_cfg)`
		- (optional) mark the current writer bank as invalid because it will be overwritten

- `swap_in=1` → toggle `bank_sel` (still `IDLE`)

### 4.2 RUN

- *fire* (`ps_data.valid_in && ps_data.ready_out`) →
	- Actions:
		- assert `dm_write_en=1`
		- write `dm_write_addr = dma_base + dma_offset`
		- `dma_offset += stride * 2 bytes` (for fp16 element size)
		- `dma_len_rem--`
	- If `dma_len_rem` becomes 0:
		- `dma_loops_rem--`
		- mark the current writer bank valid (optional bookkeeping)
		- transition to `WAIT_SWAP`

### 4.3 WAIT_SWAP

- `swap_in=1` (SWAPDM executes) →
	- Actions:
		- `bank_sel = !bank_sel`
	- If `dma_loops_rem > 0`:
		- start next phase:
			- `dma_offset = 0`
			- `dma_len_rem = dma_len_cfg`
			- clear validity of the new writer bank (optional bookkeeping)
		- transition to `RUN`
	- Else (`dma_loops_rem == 0`):
		- transition to `FINISH` (or directly to `IDLE` if no `done` pulse is needed)

### 4.4 FINISH

- Unconditional → `IDLE` (and `done` is pulsed for one cycle)

---

## 5. `SWAPDM` Early Arrival: Stall / Backpressure

Synchronization with `SWAPDM` is implemented in EXE_M:

- When the decoded instruction is `is_swap=1` and SDMA is still executing a phase (`sdma.busy=1`), EXE_M asserts a stall (e.g., `swap_stall`).
- The stall holds EXE_M (and propagates backpressure upstream) until SDMA completes the current phase.
- Once SDMA transitions to `WAIT_SWAP`, SDMA deasserts `busy`, EXE_M releases the stall, and `swap_in` can be observed by SDMA in `WAIT_SWAP` to swap banks and either start the next phase or finish the task.

---

## 6. Related Files

- SDMA implementation:
	- [design/hybridacc-ESL/simulator/include/PE/SDMA.hpp](../../simulator/include/PE/SDMA.hpp)
- `SWAPDM` stall site (`swap_stall`):
	- [design/hybridacc-ESL/simulator/include/PE/EXE_M_stage.hpp](../../simulator/include/PE/EXE_M_stage.hpp)
- DM bank role mapping (`bank_sel` semantics):
	- [design/hybridacc-ESL/simulator/include/PE/DataMemory.hpp](../../simulator/include/PE/DataMemory.hpp)
