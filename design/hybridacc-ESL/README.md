# HybridAcc ESL Simulator

This project contains the Electronic System Level (ESL) simulator for the **HybridAcc** heterogeneous accelerator, targeting efficient neural network inference (CNN / RNN). The simulator is implemented in **SystemC** with C++17.

---

## Table of Contents

- [System Architecture](#system-architecture)
- [Project Structure](#project-structure)
- [Key Components](#key-components)
- [Prerequisites](#prerequisites)
- [Building the Simulator](#building-the-simulator)
- [Running Simulations](#running-simulations)
- [Cluster Simulation Cases](#cluster-simulation-cases)
- [Cluster Simulation Script](#cluster-simulation-script)
- [Test Framework](#test-framework)
- [Debug Options](#debug-options)
- [Debug Runbook](#debug-runbook)
- [Documentation](#documentation)
- [Troubleshooting](#troubleshooting)

---

## System Architecture

HybridAcc is a scalable heterogeneous accelerator built on a **dual-plane Network-on-Chip (NoC)** that interconnects multiple Compute Clusters / Process Elements (PEs).

```
                Host / DMA
                    │
          ┌─────────▼──────────┐
          │   HybridDataDeliver │  ← HDDU (AGU × 4, SPM × 4)
          │        Unit         │
          └─────────┬───────────┘
                    │ PS / PD / PLI / PLO
          ┌─────────▼───────────┐
          │     NoCRouter       │  ← 4-channel split architecture
          │  (NoC-0 Push Plane  │
          │   NoC-1 Local Plane)│
          └──┬──────┬──────┬────┘
             │      │      │   (NUM_PORTS MBUS instances)
         ┌───▼──┐ ┌─▼───┐ ┌▼────┐
         │MBUS 0│ │MBUS1│ │MBUS2│
         └───┬──┘ └──┬──┘ └──┬──┘
       ┌─────▼──┐  ┌─▼────┐  ...
       │PE[0][0]│  │PE[1] │       ← 3-stage pipeline (IF_ID / EXE_M / EXE_A)
       │PE[0][1]│  │...   │
       └────────┘  └──────┘
         Local Network (ring)
```

### Dual-Plane NoC

| Plane | Direction | Purpose |
|-------|-----------|---------|
| **NoC-0 (Push Plane)** | Write-only | Broadcast commands, distribute weights & activations |
| **NoC-1 (Local Plane)** | Read/Write | PE-to-PE communication, partial sum collection |

### 4-Channel Data Architecture

| Channel | Direction | Data Type |
|---------|-----------|-----------|
| **PS** (Port Static) | NoC → PE | Weights / constants (64-bit) |
| **PD** (Port Dynamic) | NoC → PE | Input activations (16-bit) |
| **PLI** (Port Local Input) | NoC → PE | Partial sums from previous PE (64-bit) |
| **PLO** (Port Local Output) | PE → NoC | Partial sums out (64-bit) |

---

## Project Structure

```
hybridacc-ESL/
├── simulator/
│   ├── include/                  # Header files (Core logic – header-only design)
│   │   ├── HybridAcc.hpp         # Top-level accelerator module
│   │   ├── NetworkOnChip.hpp     # NoC top-level (parametric)
│   │   ├── ComputeCluster.hpp    # Compute Cluster module
│   │   ├── ComputeCore.hpp       # Core module
│   │   ├── ProcessElement.hpp    # PE top-level
│   │   ├── FIFO.hpp              # Synchronous FIFO primitive
│   │   ├── async_FIFO.hpp        # Asynchronous FIFO primitive
│   │   ├── utils.hpp             # Common types and utilities
│   │   ├── AXI4_lite/
│   │   │   └── axi4-lite.hpp     # AXI4-Lite bus interface
│   │   ├── Cluster/
│   │   │   ├── HybridDataDeliverUnit.hpp  # HDDU (AGU + SPM bridge)
│   │   │   ├── AddressGenerateUnit.hpp    # AGU for strided DMA
│   │   │   ├── ScratchpadMemory.hpp       # On-chip SPM model
│   │   │   └── SRAM.hpp                   # Generic SRAM model
│   │   ├── Core/
│   │   │   ├── CoreucodeIM.hpp    # Micro-code instruction memory
│   │   │   ├── Decoder.hpp        # Instruction decoder
│   │   │   └── PEucodeLoader.hpp  # PE micro-code loader
│   │   ├── NoC/
│   │   │   ├── NoCRouter.hpp     # Central router (4-channel)
│   │   │   └── MBUS.hpp          # Multicast bus (1 port → N PEs)
│   │   ├── PE/
│   │   │   ├── IF_ID_stage.hpp   # Instruction Fetch & Decode stage
│   │   │   ├── EXE_M_stage.hpp   # Multiply / Memory stage
│   │   │   ├── EXE_A_stage.hpp   # Accumulate stage
│   │   │   ├── VMULU.hpp         # Vector Multiply Unit (FP16)
│   │   │   ├── VADDU.hpp         # Vector Add/Accumulate Unit (FP16)
│   │   │   ├── PErouter.hpp      # PE-side router (NoC ↔ PE ports)
│   │   │   ├── SDMA.hpp          # Scratchpad DMA
│   │   │   ├── LDMA.hpp          # Local DMA
│   │   │   ├── DataMemory.hpp    # PE local data memory
│   │   │   ├── InstructionMemory.hpp  # PE instruction memory
│   │   │   ├── Decoder.hpp       # PE instruction decoder
│   │   │   ├── LoopController.hpp # Zero-overhead loop controller
│   │   │   ├── PsumRegFile.hpp   # Partial-sum register file
│   │   │   └── TransformRegFile.hpp  # Transform coefficient register file
│   │   └── Utils/                # (reserved for utility modules)
│   └── src/
│       ├── main.cpp              # Simulation entry point
│       └── utils.cpp             # Utility implementations
├── test/                         # Testbenches and simulation entry points
│   ├── CMakeLists.txt
│   ├── README.md                 # Detailed test framework documentation
│   ├── tb_utils.hpp              # Common testbench utilities
│   ├── mvp_compiler.hpp          # Minimal program compiler helper
│   ├── test_agu_unit.cpp         # AGU unit test
│   ├── test_spm_unit.cpp         # SPM unit test
│   ├── test_sram_unit.cpp        # SRAM unit test
│   ├── test_hddu_unit.cpp        # HDDU unit test
│   ├── test_pe_unit.cpp          # PE unit test
│   ├── test_pe_sim.cpp           # PE simulation (Conv3×3)
│   ├── test_cluster_unit.cpp     # Cluster unit test
│   ├── test_cluster_sim.cpp      # Full cluster simulation
│   ├── test_cluster_sim_advanced.cpp  # Advanced cluster simulation
│   ├── test_noc_unit.cpp         # NoC unit test (4 PEs, 10 test cases)
│   └── test_noc_sim.cpp          # Full NoC + PE system simulation
├── doc/                          # Component documentation
│   ├── HybridAcc.md              # System architecture overview
│   ├── NetworkOnChip.md          # NoC detailed specification
│   ├── ProcessElement.md         # PE pipeline specification
│   ├── HDDU.md                   # HDDU v4 specification
│   ├── AGU.md                    # Address Generate Unit specification
│   ├── ComputeCluster.md         # Compute Cluster specification
│   ├── CoreController.md         # Core Controller specification
│   ├── DMA.md                    # DMA specification
│   ├── PE_SDMA.md                # PE SDMA specification
│   ├── SPM.md                    # Scratchpad Memory specification
│   ├── SRAM.md                   # SRAM model specification
│   ├── Components.md             # Utility components specification
│   └── coding_convention.md      # C++ coding guidelines
├── plan/                         # Design plans and ISA notes
│   ├── plan.md
│   ├── core_ISA.md
│   ├── core_ISA_v2.md
│   └── cluster_sim_upgrade.md
└── CMakeLists.txt                # Top-level build configuration
```

---

## Key Components

### 1. Network-on-Chip (`NetworkOnChip<NUM_PORTS, PORT_WIDTH_BITS, NUM_PES_PER_PORT>`)

The top-level simulation container. Instantiates one `NoCRouter`, `NUM_PORTS` `MBUS` instances, and `NUM_PORTS × NUM_PES_PER_PORT` PE instances.

- **NoCRouter**: Central 4-channel routing unit with configurable FIFO depth.
- **MBUS (Multicast Bus)**: Connects one router port to N PEs; handles scan-chain configuration and flow control.

### 2. Process Element (`ProcessElement`) — 3-Stage Pipeline

| Stage | Function | Key Submodules |
|-------|----------|----------------|
| **IF_ID** | Instruction Fetch & Decode | `InstructionMemory`, `Decoder`, `LoopController` |
| **EXE_M** | Multiply / Memory | `VMULU` (FP16 ×), `DataMemory`, `SDMA`, `LDMA`, `TransformRegFile` |
| **EXE_A** | Accumulate | `VADDU` (FP16 +), `PsumRegFile` |

The `PErouter` bridges the NoC planes and the PE's PS / PD / PLI / PLO ports, with four configurable routing modes:

| Mode | PLI source | PLO destination |
|------|-----------|-----------------|
| `PLI_FROM_LN_PLO_TO_LN` | Local Network | Local Network |
| `PLI_FROM_BUS_PLO_TO_LN` | MBUS | Local Network |
| `PLI_FROM_LN_PLO_TO_BUS` | Local Network | MBUS |
| `PLI_FROM_BUS_PLO_TO_BUS` | MBUS | MBUS |

### 3. Hybrid Data Deliver Unit (`HybridDataDeliverUnit<SPM_ADDR_BITS, NOC_TAG_BITS, DATA_BITS>`)

Acts as the bridge between on-chip SRAM / DMA and the NoC. Key features:

- **4 independent AGUs** (PS / PD / PLI / PLO) for strided address generation
- **4 SPM ports** (ports 0–2 read, port 3 write)
- **MMIO control interface** for host configuration
- **Interrupt output** on error or completion

### 4. NoC Commands

| Command | Value | Description |
|---------|-------|-------------|
| `CMD_RESET` | 0 | Clear PE registers |
| `CMD_INIT` | 1 | Initialize configuration |
| `CMD_LOAD_PROGRAM` | 2 | Load instruction memory |
| `CMD_STOP_PE` | 3 | Stop PE execution |
| `CMD_START_PE` | 4 | Start PE execution |
| `CMD_NOC_SCAN_CHAIN` | 8 | Configure PE router via scan-chain |

---

## Prerequisites

| Dependency | Minimum Version | Notes |
|------------|----------------|-------|
| **CMake** | 3.16 | |
| **GCC / Clang** | C++17 support | Tested with GCC 9+ |
| **SystemC** | 2.3.3 | Located at `libs/systemc-2.3.3/` in workspace root |

---

## Building the Simulator

### Top-Level Build

```bash
cd design/hybridacc-ESL
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### Test Suite Build

The test suite has its own CMake project under `test/`:

```bash
cd design/hybridacc-ESL/test
mkdir -p build && cd build

# Basic build (no debug output)
cmake ..
make -j$(nproc)
```

> **SystemC path**: The build system looks for SystemC at `../../../libs/systemc-2.3.3`. Override with:
> ```bash
> cmake -DSYSTEMC_HOME=/path/to/systemc ..
> ```

---

## Running Simulations

All executables are placed in `test/build/` after a successful build.

### Unit Tests

```bash
cd test/build

./test_agu_unit        # AGU unit test
./test_spm_unit        # SPM unit test
./test_sram_unit       # SRAM unit test
./test_hddu_unit       # HDDU unit test
./test_pe_unit         # PE unit test (6 test cases)
./test_noc_unit        # NoC unit test (10 test cases, 4-PE config)
./test_cluster_unit    # Cluster unit test
```

### System-Level Simulations

```bash
./test_pe_sim          # PE simulation – Conv3×3 workload
./test_cluster_sim     # Full Cluster simulation
./test_cluster_sim_advanced  # Advanced Cluster simulation
./test_noc_sim         # Full NoC + PE system simulation
```

### Using CMake Targets / CTest

```bash
# Custom make targets
make run_tests         # PE unit tests
make run_sim           # PE Conv3×3 simulation
make run_noc_test      # NoC unit tests
make run_cluster_sim   # Cluster simulation
make run_cluster_sim_advanced  # Advanced cluster simulation
make run_noc_sim       # NoC + PE simulation
make run_all_tests     # All unit tests (PE + NoC)

# CTest
ctest                  # Run all registered tests
ctest -V               # Verbose output
ctest -R PE_Unit       # Filter: PE unit tests only
ctest -R NoC_Unit      # Filter: NoC unit tests only
ctest -R Conv3x3       # Filter: Conv3×3 simulation only
ctest -R ComputeCluster  # Filter: Cluster tests/simulations
```

---

## Cluster Simulation Cases

This section is generated from `testbench/cluster/*/config.json`.

Refresh command:

```bash
./scripts/update_cluster_cases_readme.sh
```

<!-- BEGIN_CLUSTER_CASES_AUTO -->
- `conv_k1c12`
- `conv_k3c4`
- `conv_k3c4ich16och16`
- `conv_k3c4ich16och64`
- `conv_k3c4ich16och64s`
- `conv_k3c4ich256och16`
- `conv_k3c4ich4och16`
- `conv_k3c4ich4och16s`
- `conv_k3c4ich4och64`
- `conv_k3c4ich4och64s`
- `conv_k3c4ich64och16`
- `conv_k3c4ich8och16`
- `conv_k3c4ich8och16s`
- `gemm_ultra_w16`
<!-- END_CLUSTER_CASES_AUTO -->

Each case directory should provide at least `config.json` and program/input files required by the simulator.

---

## Cluster Simulation Script

Use `scripts/cluster_sim.sh` for end-to-end cluster simulation build/run flows.

```bash
# Build test_cluster_sim and test_cluster_sim_advanced
./scripts/cluster_sim.sh build

# Run one case (normal simulator)
./scripts/cluster_sim.sh run -d testbench/cluster conv_k3c4

# Run one case (advanced simulator)
./scripts/cluster_sim.sh run --advanced -d testbench/cluster conv_k3c4

# Run all cases under a directory
./scripts/cluster_sim.sh run-all -d testbench/cluster

# Run all cases with advanced simulator + trace output
./scripts/cluster_sim.sh run-all --advanced -f -d testbench/cluster
```

Common options:

- `-v`: verbose mode
- `-f`: enable trace output (`trace-cluster-<tb>.json`)
- `--trace-file <path>`: explicit trace file output path
- `--clock-period <ns>`: override simulation clock period
- `--timeout-cycles <n>`: override timeout cycles

---

## Test Framework

### NoC Test Cases (`test_noc_unit`)

Configuration: **2 ports × 2 PEs/port = 4 PEs total**, clock period 10 ns.

| # | Test Case | Description |
|---|-----------|-------------|
| 1 | Reset & Init | System reset and initial state verification |
| 2 | Scan-Chain Config | Configure PS/PD/PLI/PLO IDs and routing modes for all PEs |
| 3 | CMD_RESET | Broadcast reset command; verify all PEs respond |
| 4 | CMD_INIT | Broadcast init command; verify parameter propagation |
| 5 | Program Loading | Load instruction memory to all PEs via NoC |
| 6 | Start PE | CMD_START_PE; verify synchronized PE launch |
| 7 | Stop PE | CMD_STOP_PE; verify all PEs halt |
| 8 | Router Modes | Exercise all 4 `PERouterMode` configurations |
| 9 | Sequential Execution | Full flow: config → load → run → stop |
| 10 | NoC Data Transfer | PS and PD channel read/write verification |

### PE Instruction Encoding

```cpp
const uint16_t NOP_INST  = 0x0004;  // opcode=2, funct2=0
const uint16_t HALT_INST = 0x001E;  // opcode=3, funct2=3
```

See [PE ISA documentation](../hybridacc-pe-isa/doc/ISA.md) for the full instruction set.

---

## Debug Options

The test project currently enables debug output via `ENABLE_DEBUG_UTILS` and debug level range settings:

| CMake Option | Scope | Description |
|---|---|---|
| `-DENABLE_DEBUG_UTILS=ON` | All modules | General utility debug messages |
| `-DDEBUG_LEVEL_MIN=<n>` | All modules | Lower bound of enabled debug level |
| `-DDEBUG_LEVEL_MAX=<n>` | All modules | Upper bound of enabled debug level |

```bash
# Reconfigure test build with debug output
cd test/build
cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=0 -DDEBUG_LEVEL_MAX=6 ..
make -j$(nproc)

# Capture full log
./test_noc_unit 2>&1 | tee noc_test.log
```

---

## Debug Runbook

### 1) Fast local triage (single case)

```bash
# Build cluster simulators
./scripts/cluster_sim.sh build

# Run one case with verbose + trace
./scripts/cluster_sim.sh run --advanced -v -f -d testbench/cluster conv_k3c4
```

Check outputs:

- run log: `output/out-cluster-conv_k3c4.log`
- trace: `output/trace-cluster-conv_k3c4.json` (or custom `--trace-file`)

### 2) Batch regression triage

```bash
./scripts/cluster_sim.sh run-all --advanced -d testbench/cluster
```

When failures happen, inspect the last lines first:

```bash
tail -n 80 output/out-cluster-<tb_name>.log
```

### 3) Module-level debug rebuild

```bash
cd design/hybridacc-ESL/test/build
cmake -DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=0 -DDEBUG_LEVEL_MAX=6 ..
make -j$(nproc)
```

### 4) GDB quick attach (cluster simulation)

```bash
cd design/hybridacc-ESL/test/build
gdb ./test_cluster_sim_advanced
(gdb) run -d ../../../testbench/cluster/conv_k3c4 --no-dry-run --clock-period 1 --timeout-cycles 200
(gdb) backtrace
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [doc/HybridAcc.md](doc/HybridAcc.md) | System architecture overview |
| [doc/NetworkOnChip.md](doc/NetworkOnChip.md) | NoC detailed specification |
| [doc/ProcessElement.md](doc/ProcessElement.md) | PE pipeline specification |
| [doc/HDDU.md](doc/HDDU.md) | HDDU v4 specification |
| [doc/AGU.md](doc/AGU.md) | Address Generate Unit specification |
| [doc/ComputeCluster.md](doc/ComputeCluster.md) | Compute Cluster specification |
| [doc/CoreController.md](doc/CoreController.md) | Core Controller specification |
| [doc/DMA.md](doc/DMA.md) | DMA specification |
| [doc/SPM.md](doc/SPM.md) | Scratchpad Memory specification |
| [doc/Components.md](doc/Components.md) | Utility components (AGU, SRAM, Decoder) |
| [doc/coding_convention.md](doc/coding_convention.md) | C++ coding guidelines |
| [test/README.md](test/README.md) | Test framework detailed guide |

---

## Troubleshooting

### Build Issues

| Problem | Fix |
|---------|-----|
| CMake cannot find SystemC | Set `-DSYSTEMC_HOME=/path/to/systemc` or export `SYSTEMC_HOME` |
| `NetworkOnChip.hpp` not found | Verify `simulator/include/` path in `CMakeLists.txt` |
| Linker `undefined reference` | `cd build && rm -rf * && cmake .. && make` |

### Runtime Issues

| Problem | Fix |
|---------|-----|
| `libsystemc.so` not found at runtime | `export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:../../../libs/systemc-2.3.3/lib-linux64` |
| NoC/Cluster test timeout | Rebuild with `-DENABLE_DEBUG_UTILS=ON -DDEBUG_LEVEL_MIN=0 -DDEBUG_LEVEL_MAX=6`, then inspect `output/out-cluster-*.log` and trace JSON |
| PE does not start | Confirm `CMD_START_PE` and `router_enable=true`, then rerun with `./scripts/cluster_sim.sh run --advanced -v -f ...` |
| SystemC elaboration error | Check for unbound ports or duplicate module names |

### GDB Quick-Start

```bash
gdb ./test_noc_unit
(gdb) run
(gdb) backtrace
```
