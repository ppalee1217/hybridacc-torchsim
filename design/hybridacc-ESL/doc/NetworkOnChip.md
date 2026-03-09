# Network-on-Chip (NoC) Architecture

## Overview

The Network-on-Chip (NoC) in HybridAcc provides high-bandwidth, low-latency communication between the host/memory and the Processing Elements (PEs). It employs a **4-channel split architecture** (PS / PD / PLI / PLO) to maximise throughput and minimise head-of-line blocking across different traffic types.

The top-level module `NetworkOnChip<NUM_PORTS, PORT_WIDTH_BITS, NUM_PES_PER_PORT>` instantiates one `NoCRouter`, `NUM_PORTS` instances of `MBUS`, and `NUM_PORTS × NUM_PES_PER_PORT` PE instances.

```
                    ┌─────────────────────────────────────────┐
 noc_ps_in ──────►  │                                         │
 noc_pd_in ──────►  │           NoCRouter                     │  ── scan_chain_out[i] ──►  MBUS[i]
 noc_pli_in ─────►  │   (4 input FIFOs + 1 response FIFO)    │  ◄─ scan_chain_in[i]  ──   MBUS[i]
 noc_plo_in ─────►  │                                         │
 noc_plo_out ◄────  │                                         │
                    └─────────────────────────────────────────┘
                          │ noc_{ps,pd,pli,plo}_to_bus_req[i]
                          │ bus_to_noc_plo_resp[i]
                          ▼  (per port i = 0 … NUM_PORTS-1)
                    ┌──────────┐
                    │  MBUS[i] │  ── router_enable/mode[j] ──► PE[i][j]
                    │          │  ── bus_to_pe_{ps,pd,pli,plo}_req[j] ──► PE[i][j]
                    │          │  ◄─ pe_to_bus_plo_resp[j]             ── PE[i][j]
                    └──────────┘
                          │ ln_pli_plo[i][j]  (Local Network, PE-to-PE)
                          ▼
                    PE[i][j]  ←→  PE[i][j+1]  (ring)
```

---

## 4-Channel Architecture

| Channel | Direction       | Purpose                                  |
|---------|-----------------|------------------------------------------|
| **PS**  | NoC → PE (Write) | Weight distribution (Parameter Store)   |
| **PD**  | NoC → PE (Write) | Input activation distribution (Parameter Data) |
| **PLI** | NoC → PE (Write) | Partial-sum write-in (or Local Network forwarding) |
| **PLO** | PE → NoC (Read)  | Partial-sum read-out (Request + Response) |

*   **PS / PD / PLI** are unidirectional push channels (write-only from the NoC perspective).
*   **PLO** is a pull channel: the host issues a read-address request (`noc_plo_in`), the router fans it out to the target PE(s) via MBUS, and collects the response back to `noc_plo_out`.

---

## Components

### 1. NoCRouter

**Template parameters**

| Parameter | Default | Description |
|-----------|---------|-------------|
| `NUM_PORTS` | 3 | Number of MBUS ports (downstream) |
| `PORT_WIDTH_BITS` | 64 | Per-port data width in bits |

The total upstream data width for PS/PD/PLI channels is `NUM_PORTS × PORT_WIDTH_BITS` bits.

**External ports**

| Port | Type | Direction | Description |
|------|------|-----------|-------------|
| `clk` / `reset_n` | `bool` | in | Clock / active-low reset |
| `command_mode` | `bool` | in | Sideband command strobe |
| `command_data` | `sc_uint<32>` | in | Sideband command payload |
| `noc_ps_in` | `VRDIF<request_t<sc_biguint<N×W>, uint16_t>>` | in | Weight write requests |
| `noc_pd_in` | same | in | Activation write requests |
| `noc_pli_in` | same | in | Partial-sum write requests |
| `noc_plo_in` | `VRDIF<noc_addr_req_t>` | in | Partial-sum read-address requests |
| `noc_plo_out` | `VRDOF<response_t<sc_biguint<N×W>>>` | out | Collected read responses |
| `noc_{ps,pd,pli}_to_bus_req[i]` | `VRDOF<noc_request_t>` | out | Routed write requests to MBUS[i] |
| `noc_plo_to_bus_req[i]` | `VRDOF<noc_addr_req_t>` | out | Routed read requests to MBUS[i] |
| `bus_to_noc_plo_resp[i]` | `VRDIF<noc_response_t>` | in | Read responses from MBUS[i] |
| `scan_chain_enable` | `bool` | out | Broadcast enable to all MBUS |
| `scan_chain_in[i]` | `ScanChainFormat` | in | From MBUS[i] scan-chain output |
| `scan_chain_out[i]` | `ScanChainFormat` | out | To MBUS[i] scan-chain input |

**Internal FIFOs** (configurable depth via `noc_fifo_depth`)

| FIFO | Payload type |
|------|-------------|
| `ps_fifo` | `request_t<sc_biguint<N×W>, uint16_t>` |
| `pd_fifo` | same |
| `pli_fifo` | same |
| `plo_fifo` | `noc_addr_req_t` |
| `resp_fifo` | `response_t<sc_biguint<N×W>>` |

**Dispatch modes**

Each incoming wide request carries an address field:

```
addr[15:8] – reserved
addr[7]    – command flag (input level; overridden to addr[6] after router decode)
addr[6]    – ultra/SIMD mode  (0 = Broadcast, 1 = SIMD)
addr[5:0]  – tag (PE channel ID)
```

*   **Broadcast mode** (`addr[6] = 0`): All `NUM_PORTS` MBUS ports receive the same 64-bit slice `data[63:0]`.
*   **SIMD / Ultra mode** (`addr[6] = 1`): Port `i` receives slice `data[64×i+63 : 64×i]`.

**After decoding**, the address forwarded downstream is:
```
out_addr[6] = command flag (from input addr[7])
out_addr[5:0] = tag (from input addr[5:0])
```

**Sideband command (`command_mode` / `command_data`)**

Commands arrive on a dedicated sideband path that bypasses the FIFOs. The 4-bit command code is in `command_data[3:0]`:

| Code | Name | Description |
|------|------|-------------|
| 0 | `CMD_RESET` | Clear PE registers |
| 1 | `CMD_INIT` | Set PE IDs, mode, enable |
| 2 | `CMD_LOAD_PROGRAM` | Load program to PE instruction memory |
| 3 | `CMD_STOP_PE` | Stop PE operation |
| 4 | `CMD_START_PE` | Start PE operation |
| 8 | `CMD_NOC_SCAN_CHAIN` | Scan-chain shift (handled by router only) |

PE commands (codes 0–4) are injected directly into all `NUM_PORTS` PS channels with `addr = 0x40` (command bit set), bypassing the PS FIFO and taking highest priority.

Scan-chain commands (code 8) update the router's internal `scan_chain_data_reg` and assert `scan_chain_enable` for one clock cycle.

**PLO read flow**

1. `noc_plo_in` accepted → pushed into `plo_fifo`.
2. `process_requests_noc_plo` fans the address out to all ports.
3. `pending_read_reg` is asserted; further PLO requests are stalled (`rx_stall_sig`).
4. `process_responses_plo` waits for responses from MBUS:
   - **Broadcast**: expects exactly one valid response from among all ports.
   - **SIMD**: expects one valid response from **each** port; they are concatenated into the wide response.
5. Combined response pushed into `resp_fifo` → forwarded to `noc_plo_out`.

**Sequential registers**

| Register | Description |
|----------|-------------|
| `scan_chain_data_reg` | Current scan-chain data to shift out |
| `scan_chain_enable_reg` | Latched scan-chain enable |
| `pending_read_reg` | PLO read in-flight flag |
| `pending_read_ultra_reg` | PLO in-flight is SIMD mode |

---

### 2. MBUS (Multicast Bus)

One `MBUS` instance per router port. It connects one `NoCRouter` port to `num_pes` `ProcessElement` instances (default 16).

**External ports**

| Port | Type | Direction | Description |
|------|------|-----------|-------------|
| `clk` / `reset_n` | `bool` | in | Clock / active-low reset |
| `scan_chain_enable` | `bool` | in | Shift-register enable from router |
| `scan_chain_in` | `ScanChainFormat` | in | Data from router scan-chain output |
| `scan_chain_out` | `ScanChainFormat` | out | Data to router scan-chain input |
| `noc_{ps,pd,pli}_to_bus_req` | `VRDIF<noc_request_t>` | in | Write requests from router |
| `noc_plo_to_bus_req` | `VRDIF<noc_addr_req_t>` | in | Read address from router |
| `bus_to_noc_plo_resp` | `VRDOF<noc_response_t>` | out | Collected response to router |
| `router_enable[j]` | `bool` | out | Enable signal to PE[j] |
| `router_mode[j]` | `PERouterMode` | out | Routing mode to PE[j] |
| `bus_to_pe_{ps,pd,pli}_req[j]` | `VRDOF<noc_request_t>` | out | Write to PE[j] |
| `bus_to_pe_plo_req[j]` | `VRDOF<noc_addr_req_t>` | out | Read req to PE[j] |
| `pe_to_bus_plo_resp[j]` | `VRDIF<noc_response_t>` | in | Response from PE[j] |
| `pe_busy[j]` | `bool` | in | PE[j] busy status |

**Scan-chain shift register**

Each MBUS maintains `num_pes` copies of `ScanChainFormat` stored in `pe_scan_chain_signals_reg[]`. When `scan_chain_enable` is asserted, new data shifts in from `scan_chain_in` (LSB = PE[0]) and the previous tail value shifts out to `scan_chain_out`. This allows the router to program all PE configurations by serially shifting data through:

```
Router → scan_chain_out[i] → MBUS[i].scan_chain_in
                              shift register: PE[0]…PE[N-1]
                              MBUS[i].scan_chain_out → Router.scan_chain_in[i]
```

**ScanChainFormat per PE**

| Field | Type | Description |
|-------|------|-------------|
| `ps_id` | `uint8_t` | Tag ID for PS channel (matched against `addr[5:0]`) |
| `pd_id` | `uint8_t` | Tag ID for PD channel |
| `pli_id` | `uint8_t` | Tag ID for PLI channel |
| `plo_id` | `uint8_t` | Tag ID for PLO channel |
| `route_mode` | `PERouterMode` | PLI/PLO routing mode |
| `enable` | `bool` | PE enabled in this MBUS |

**Tag-based routing**

For each write channel (PS/PD/PLI/PLO), MBUS computes a 64-bit `target_mask` from the stored PE configurations:

```
if addr[6] == 1 (command):  mask = all enabled PEs
else:                        mask |= (1 << j) if pe_scan_chain[j].<ch>_id == addr[5:0]
```

The channel data is forwarded with `valid` only to PEs whose bit is set in the mask. The upstream `ready` signal is asserted only when **all** masked PEs signal ready (full multicast semantics).

During scan-chain mode (`scan_chain_enable = 1`), all routing is disabled (`mask = 0`, `ready = 0`).

**PLO response arbitration**

When a PLO read request is accepted, `rx_mask_next` is set to the computed target mask. On the next clock, `rx_mask_reg` records which PEs are expected to respond. `comb_pe_to_noc_plo_response` scans all PEs in `rx_mask_reg`:
- Zero valid responses → `active = false`.
- Exactly one valid response → forward that response.
- More than one valid response → `status = NOC_ERROR`.

**PERouterMode**

Controls whether PE[j]'s PLI input and PLO output are routed through the MBUS or through the Local Network:

| Value | Mode | PLI source | PLO dest |
|-------|------|-----------|----------|
| `0b00` | `PLI_FROM_LN_PLO_TO_LN` | Local Network | Local Network |
| `0b01` | `PLI_FROM_BUS_PLO_TO_LN` | MBUS | Local Network |
| `0b10` | `PLI_FROM_LN_PLO_TO_BUS` | Local Network | MBUS |
| `0b11` | `PLI_FROM_BUS_PLO_TO_BUS` | MBUS | MBUS |

---

### 3. NetworkOnChip (Top-level)

**Template parameters**

| Parameter | Description |
|-----------|-------------|
| `NUM_PORTS` | Number of MBUS / router ports |
| `PORT_WIDTH_BITS` | Per-port data width (bits) |
| `NUM_PES_PER_PORT` | PEs per MBUS |

Total PE count = `NUM_PORTS × NUM_PES_PER_PORT`.

**Local Network (PE-to-PE)**

`ln_pli_plo[NUM_PORTS+1][NUM_PES_PER_PORT]` forms a chained ring between PE columns. Each PE[i][j] connects:
- `pe_inst.ln_pli` → `ln_pli_plo[i][j]`  (receive from upstream column)
- `pe_inst.ln_plo` → `ln_pli_plo[i+1][j]`  (send to downstream column)

This allows systolic-array-style partial-sum accumulation across PE columns.

**Perfetto trace support**

`enable_perffeto_trace(start_pid, start_tid)` assigns unique process/thread IDs to:
1. NoCRouter (40 trace threads).
2. Each MBUS (7 trace threads each).
3. Each PE (variable trace threads).

---

## Communication Protocol

All inter-module interfaces use the **Valid-Ready handshake** protocol defined by three template structs:

| Struct | Role | Signals |
|--------|------|---------|
| `VRDIF<T>` | Receiver / slave | `data_in`, `valid_in`, `ready_out` |
| `VRDOF<T>` | Sender / master | `data_out`, `valid_out`, `ready_in` |
| `VRDSIG<T>` | Internal wire bundle | `data_sig`, `valid_sig`, `ready_sig` |

A transfer occurs when **both** `valid` and `ready` are asserted in the same clock cycle.

**Data types**

| Type | Definition | Fields |
|------|-----------|--------|
| `noc_request_t` | `request_t<uint64_t, uint16_t>` | `data` (64 b), `addr` (16 b), `mask` |
| `noc_addr_req_t` | struct | `addr` (16 b) |
| `noc_response_t` | `response_t<uint64_t>` | `data` (64 b), `status` |
| Wide request (at router input) | `request_t<sc_biguint<N×W>, uint16_t>` | `data` (N×W b), `addr` (16 b), `mask` |

**NOC_RESPONSE_STATUS**

| Value | Meaning |
|-------|---------|
| `NOC_OK` (0) | Successful read |
| `NOC_ERROR` (1) | Collision / multiple-responder error |
| `NOC_NOP` (2) | No operation / default |

---

## Scan Chain

The scan chain provides a serial configuration path for programming PE identifiers and routing modes without consuming NoC bandwidth.

**Flow**

1. Host sends `CMD_NOC_SCAN_CHAIN` via `command_mode`/`command_data`.
2. NoCRouter latches the `ScanChainFormat` payload and asserts `scan_chain_enable`.
3. Each MBUS shifts the data through its PE configuration registers (`pe_scan_chain_signals_reg`).
4. The tail of each MBUS scan chain feeds back to the router's `scan_chain_in`, daisy-chaining all MBUSes.
5. When the host de-asserts `command_mode`, `scan_chain_enable` is cleared and all routing resumes normally.

**ScanChainFormat fields** (see table in MBUS section above).
