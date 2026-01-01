# Process Element (PE) Architecture

## Overview

The Process Element (PE) is the fundamental computation unit of the HybridAcc accelerator. It is designed as a 3-stage pipeline to efficiently execute vector operations for neural networks.

## Pipeline Stages

### 1. IF_ID Stage (Instruction Fetch & Decode)
*   **Function**: Fetches instructions from the Instruction Memory (IM), decodes them, and handles loop control.
*   **Components**:
    *   `InstructionMemory`: Stores the PE program.
    *   `Decoder`: Decodes instructions into control signals.
    *   `LoopController`: Manages hardware loops (Zero-Overhead Loops).
*   **Outputs**: Decoded signals to the EXE_M stage.

### 2. EXE_M Stage (Execution - Memory/Multiply)
*   **Function**: Performs vector multiplication and handles data movement.
*   **Components**:
    *   `VMULU` (Vector Multiply Unit): Performs FP16 vector multiplication.
    *   `DataLoader`: Loads data from Data Memory or external ports.
    *   `DataMemory`: Local scratchpad memory.
    *   `TransformRegFile`: Register file for transformation coefficients.
*   **Inputs**:
    *   **PS (Port Static)**: Input for stationary data (e.g., weights).
    *   **PD (Port Dynamic)**: Input for streaming data (e.g., activations).
*   **Outputs**: Multiplication results to EXE_A stage.

### 3. EXE_A Stage (Execution - Accumulate)
*   **Function**: Performs vector accumulation and manages partial sums.
*   **Components**:
    *   `VADDU` (Vector Add Unit): Performs FP16 vector addition/accumulation.
    *   `PsumRegFile`: Register file for storing partial sums.
*   **Inputs**:
    *   **PLI (Port Local Input)**: Input from neighbor PE (Local Network).
*   **Outputs**:
    *   **PLO (Port Local Output)**: Output to neighbor PE or NoC.

## PE Router (`PErouter`)

The `PErouter` manages the data flow into and out of the PE. It connects the PE to the NoC and the Local Network.

*   **NoC Interface**:
    *   Receives data from **NoC-0** and **NoC-1**.
    *   Routes data to internal ports: `PS`, `PD`, `PLI`.
    *   Sends data from `PLO` to **NoC-1**.
*   **Control**:
    *   Handles PE configuration commands (Reset, Start, Program IM).
    *   Manages flow control (Valid/Ready) for all ports.

## Data Ports

*   **PS (Port Static)**: 64-bit wide. Typically used for weights or constants.
*   **PD (Port Dynamic)**: 16-bit wide. Typically used for input activations.
*   **PLI (Port Local Input)**: 64-bit wide. Input from the previous PE in the chain.
*   **PLO (Port Local Output)**: 64-bit wide. Output to the next PE or back to the NoC.

## Instruction Set
The PE executes a custom VLIW-like instruction set optimized for CNN/RNN operations. Instructions control the datapath, memory access, and loop logic.
