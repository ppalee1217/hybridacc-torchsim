# HybridAcc System Architecture

文件樹： [../../../../doc/index.md](../../../../doc/index.md) -> [../index.md](../index.md) -> [README.md](README.md) -> 本頁。

## Overview

HybridAcc is a heterogeneous accelerator designed for efficient neural network processing. The system is built upon a scalable Network-on-Chip (NoC) architecture that interconnects multiple Compute Clusters or Process Elements (PEs).

The current ESL (Electronic System Level) simulation focuses on the **NetworkOnChip** module as the top-level container, which instantiates the interconnect and the Processing Elements.

## System Components

### 1. Network-on-Chip (NoC)
The NoC provides the communication backbone for the accelerator. It features a **Dual-Plane Architecture**:
*   **NoC-0 (Push Plane)**: A write-only plane used for broadcasting commands, distributing weights, and streaming input activations to PEs.
*   **NoC-1 (Local Network Plane)**: A read/write plane used for PE-to-PE communication (Local Network) and collecting partial sum results.

Key components include:
*   **NoCRouter**: The central routing unit that manages traffic between different ports.
*   **MBUS (Multicast Bus)**: Connects a single router port to multiple PEs, enabling efficient data distribution and collection.

### 2. Process Element (PE)
The PE is the core computation unit, featuring a 3-stage pipeline:
*   **IF_ID Stage**: Instruction Fetch and Decode. Handles loop control and instruction dispatch.
*   **EXE_M Stage**: Execution Memory/Multiply. Performs vector multiplication (`VMULU`) and handles data loading (`DataLoader`) and storage (`DataMemory`).
*   **EXE_A Stage**: Execution Accumulate. Performs vector accumulation (`VADDU`) and manages partial sums (`PsumRegFile`).

The PE interfaces with the system via the **PErouter**, which manages data flow between the NoC planes and the PE's internal ports (PS, PD, PLI, PLO).

### 3. Hybrid Data Deliver Unit (HDDU)
The HDDU acts as a bridge between the system memory (SRAM/DMA) and the NoC. It contains Address Generation Units (AGUs) for Weights, Inputs, and Accumulation data, ensuring efficient data feeding into the accelerator.

## Simulation Structure

The simulation is built using SystemC. The top-level testbench (`test_noc_sim.cpp`) instantiates the `NetworkOnChip` module and drives it with test patterns (activations, weights, programs) loaded from files.

### Directory Structure
*   `simulator/include/`: Header files for all modules.
    *   `NoC/`: Router and MBUS definitions.
    *   `PE/`: Process Element stages and units.
    *   `Cluster/`: HDDU and memory components.
*   `simulator/src/`: Implementation files (mostly empty as logic is often in headers or specific cpp files).
*   `test/`: Testbenches and simulation entry points (`test_noc_sim.cpp`, `test_pe_sim.cpp`).
