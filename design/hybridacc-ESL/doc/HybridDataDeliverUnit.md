# Hybrid Data Deliver Unit (HDDU)

## Overview

The Hybrid Data Deliver Unit (HDDU) is a specialized memory management unit designed to efficiently feed data from the system memory (SRAM or DMA) to the Network-on-Chip (NoC). It automates address generation and data fetching for different data types (Weights, Inputs, Accumulation).

## Architecture

The HDDU consists of three independent Address Generation Units (AGUs) and an Arbiter.

### Address Generation Units (AGUs)

1.  **W_AGU (Weight AGU)**:
    *   Responsible for fetching weight data.
    *   Generates read requests to SRAM/DMA.
    *   Tags requests for routing to the correct PEs via NoC.

2.  **I_AGU (Input AGU)**:
    *   Responsible for fetching input activation data.
    *   Generates read requests to SRAM/DMA.

3.  **A_AGU (Accumulation AGU)**:
    *   Responsible for fetching/storing accumulation data (partial sums).
    *   (Note: In the current code, it generates read requests, similar to other AGUs).

### Arbiter
The Arbiter manages access to the single NoC output port (`noc_out_data`).
*   **Function**: It arbitrates between the three AGUs (`W_AGU`, `I_AGU`, `A_AGU`) to decide which one gets to send data to the NoC.
*   **Policy**: Uses a locking mechanism (`ARB_LOCKED_W`, `ARB_LOCKED_I`, `ARB_LOCKED_A`) to ensure atomic transfers or burst handling.

## Interfaces

*   **DMA/SRAM Interface**:
    *   `dma_in_data`: Data input from memory (256-bit).
    *   `sram_read_addr`: Read address output to memory.
    *   `sram_read_req`: Read request signal.
*   **NoC Interface**:
    *   `noc_out_data`: Data output to NoC (256-bit).
    *   `noc_out_addr`: Destination address/tag (10-bit).
    *   `noc_out_valid`/`noc_out_ready`: Handshake signals.
*   **MMIO Interface**:
    *   Used for configuring the AGUs (base addresses, strides, lengths) from a host processor.

## Operation
1.  **Configuration**: The host configures the AGUs via MMIO.
2.  **Request Generation**: AGUs generate memory read requests based on their configuration.
3.  **Data Fetch**: Data returns from SRAM/DMA.
4.  **Arbitration**: The Arbiter selects an active AGU.
5.  **Dispatch**: Data is sent to the NoC with the appropriate destination tag.
