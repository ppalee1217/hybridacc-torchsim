# Utility Components Specification

## Overview
This document specifies the smaller utility components used within the larger modules (HDDU, PE, Cluster).

## AddressGenerateUnit (AGU)
**Used in**: `ClusterDataDeliverUnit` (HDDU)

### Function
Generates a sequence of addresses for reading/writing data from/to SRAM. It supports linear and strided access patterns.

### Interface
- `clk`, `reset_n`
- `config_base`: Base address.
- `config_stride`: Stride value.
- `config_length`: Number of items to generate.
- `enable`: Start generation.
- `addr_out`: Generated address.
- `valid_out`: Address valid.

---

## DataMemory (SRAM)
**Used in**: `ClusterDataDeliverUnit`, `ProcessElement` (Scratchpad)

### Function
A generic single-port or dual-port SRAM model.

### Interface
- `clk`
- `ce` (Chip Enable)
- `we` (Write Enable)
- `addr`: Address bus.
- `wdata`: Write data.
- `rdata`: Read data.

---

## DataMux
**Used in**: `ClusterDataDeliverUnit`

### Function
A simple multiplexer to select data sources.

### Interface
- `sel`: Selection signal.
- `in0`, `in1`, ...: Data inputs.
- `out`: Data output.

---

## Decoder
**Used in**: `ProcessElement` (IF_ID Stage)

### Function
Decodes the 32-bit instruction word into control signals for the datapath.

### Interface
- `instr_in`: 32-bit instruction.
- `ctrl_signals_out`: Struct containing all control lines (ALU op, Reg Write, Mux Selects, etc.).
