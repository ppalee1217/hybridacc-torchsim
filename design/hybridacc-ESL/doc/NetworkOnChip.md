# Network-on-Chip (NoC) Architecture

## Overview

The Network-on-Chip (NoC) in HybridAcc is designed to provide high-bandwidth, low-latency communication between the host/memory and the Processing Elements (PEs). It employs a **Dual-Plane Architecture** to separate different types of traffic and maximize efficiency.

## Dual-Plane Architecture

### NoC-0: Push Plane (Write-Only)
*   **Purpose**: Used for high-bandwidth data distribution from the HDDU/Host to the PEs.
*   **Traffic Types**:
    *   **Commands**: Configuration and control signals.
    *   **Weights**: Neural network weights.
    *   **Input Activations**: Input feature maps.
*   **Flow**: Unidirectional, from Router to PEs.

### NoC-1: Local Network Plane (Read/Write)
*   **Purpose**: Used for inter-PE communication and result collection.
*   **Traffic Types**:
    *   **Local Network (LN)**: Data transfer between PEs (e.g., systolic array data flow).
    *   **Result Collection**: Reading partial sums or final results from PEs.
*   **Flow**: Bidirectional, supports Request/Response transactions.

## Components

### 1. NoCRouter
The `NoCRouter` is the central switching element.
*   **Ports**: Configurable number of ports (default 4).
*   **Interfaces**:
    *   `req0_in`: Input for NoC-0 requests.
    *   `req1_in`: Input for NoC-1 requests.
    *   `resp1_out`: Output for NoC-1 responses.
    *   `noc0_to_bus_req`: Output to MBUS for NoC-0.
    *   `noc1_to_bus_req`: Output to MBUS for NoC-1.
    *   `bus_to_noc1_resp`: Input from MBUS for NoC-1 responses.

### 2. MBUS (Multicast Bus)
The `MBUS` connects a single router port to a cluster of PEs (e.g., 16 PEs).
*   **Function**: It acts as a demultiplexer for incoming NoC traffic and a multiplexer/arbiter for outgoing PE traffic.
*   **Interfaces**:
    *   Connects to `NoCRouter` on one side.
    *   Connects to multiple `ProcessElement` instances on the other.
    *   Splits traffic to PEs based on the plane (NoC-0 vs NoC-1).

## Communication Protocol

The system uses a **Valid-Ready Handshake** protocol (`VRDIF` / `VRDOF`) for all interfaces to ensure reliable data transfer and flow control.

*   **Request Packet (`noc_request_t`)**:
    *   `data`: 256-bit payload.
    *   `addr`: Destination address (Channel + Tag).
    *   `mask`: Write mask.
*   **Response Packet (`noc_response_t`)**:
    *   `data`: 256-bit payload.

## Scan Chain
The NoC supports a scan chain mechanism for configuration and debugging, allowing the state of routers and MBUS modules to be programmed or read back.
