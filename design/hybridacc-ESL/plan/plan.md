# HybridAcc System Architecture & Toolchain Plan

## 1. Simulator Architecture Overview

Based on the analysis of `/design/hybridacc-ESL/simulator`, the system is structured as follows:

### **Level 1: Compute Cluster (`ComputeCluster`)**
The top-level entity that aggregates processing power and memory.
- **Components**:
    - **`ScratchpadMemory` (SPM)**: The main shared memory for the cluster. It has high-bandwidth ports connected to the NoC and external DMA.
    - **`HybridDataDeliverUnit` (HDDU)**: Manages data movement between SPM and PEs via the NoC. It handles address translation and request generation.
    - **`NetworkOnChip` (NoC)**: Interconnects the SPM/HDDU to the Processing Elements (PEs).
- **Interfaces**:
    - **Data Port (64-bit)**: `data_req_vld_i`, `data_addr_i`, `data_wdata_i` etc.
      - **Purpose**: High-speed data loading (DMA) into SPM. Used for loading input feature maps, weights, and instructions.
    - **Command Port (32-bit)**: `cmd_req_vld_i`, `cmd_addr_i`, `cmd_wdata_i` etc.
      - **Purpose**: Memory Mapped I/O (MMIO) for configuring the cluster.
        - **SPM Config**: `0x0000` - Arbitration policy, memory mapping.
        - **HDDU Config**: `0x1000` - Base addresses for instruction/data streams.
        - **NoC Config**: `0x2000` - Routing tables, expected packet sizes.

### **Level 2: Processing Element (`PE`)**
The computational unit.
- **Components**:
    - **`InstructionMemory` (IM)**: Local memory storing the program.
    - **`DataMemory`**: Local scratchpad for operands.
    - **`LDMA`/`SDMA`**: Load/Store units to fetch data from cluster memory.
    - **`ExecutionUnits`**: ALU/MAC units.
    - **`Router`**: Interface to the NoC.

---

## 2. Toolchain Proposal

Since you already have a PE-ISA compiler (likely outputting assembly or object files for a single PE), the missing pieces are **Cluster Orchestration** and **Runtime Management**.

### **Step 1: The Cluster Linker (`hyacc-ld`)**
**Role**: Combine multiple PE binaries + Data + Configuration into a single deployable image.

- **Inputs**:
    1.  **PE Binaries**: Compilation results from your existing compiler (e.g., `kernel_conv.o`, `kernel_relu.o`).
    2.  **Cluster Description File (CDF)**: A YAML/JSON file describing the physical cluster (e.g., 4x4 mesh, SPM size).
    3.  **Mapping Script**: Defines which kernel runs on which PE ID, and memory allocation requirements.

- **Process**:
    1.  **Address Resolution**: Map logical addresses in PE code to physical offsets in the cluster SPM (if using global addressing) or prepare relocation tables.
    2.  **Resource Allocation**: Assign PEs to kernels.
    3.  **Configuration Generation**: Generate register values for HDDU (e.g., "Loop 1 starts at SPM `0x4000`, send to PE 0-3").
    4.  **Packaging**: Bundle everything into a Loadable Package (see Section 3).

### **Step 2: The Runtime Driver / Loader (`hyacc-run`)**
**Role**: Execute the package on the simulator (or future FPGA/ASIC).

- **Inputs**: Loadable Package file.
- **Process**:
    1.  **Parse Header**: Read package metadata (version, entry point).
    2.  **Load Data**: Use the **Data Port (DMA)** to burst-write the binary blobs (instructions, weights, inputs) into the SPM.
    3.  **Configure Hardware**: Use the **Command Port (MMIO)** to set up the HDDU, NoC routing tables, and PE start signals.
    4.  **Manage Execution**: Monitor interrupt lines or poll status registers until completion.

---

## 3. Loadable Package Formats

Here are three proposed designs for the binary format that the Cluster Linker produces and the Driver consumes.

### **Format A: The "HAP" (HybridAcc Package) - Flat Binary Archive**
**Concept**: A simple, custom binary format designed for zero-copy loading. It mimics the hardware memory map.

#### **Structure**
```c
struct HAP_Header {
    char magic[4];       // "HAP1"
    uint32_t num_segments;
    uint32_t entry_point; // Command to start execution (or initial PC)
};

struct HAP_Segment {
    uint32_t type;       // 0=DATA (SPM), 1=CONFIG (MMIO), 2=INFO (Metadata)
    uint32_t dest_addr;  // Physical address in Simulator (SPM addr or MMIO reg)
    uint32_t file_offset;// Offset in this file where data begins
    uint32_t size;       // Size in bytes
};
// [Data Blobs follow immediately]
```

#### **Example (Hex View)**
```text
48 41 50 31  (Magic: HAP1)
00 00 00 02  (2 Segments)
00 00 00 00  (Entry Point: 0)

// Segment 1: Weights -> SPM 0x2000
00 00 00 00  (Type: DATA)
00 00 20 00  (Dest: 0x2000)
00 00 00 40  (Offset: 64 bytes)
00 00 01 00  (Size: 256 bytes)

// Segment 2: HDDU Config -> MMIO 0x1000
00 00 00 01  (Type: CONFIG)
00 00 10 00  (Dest: 0x1000)
00 00 01 40  (Offset: 320 bytes)
00 00 00 10  (Size: 16 bytes)

... [Binary Data] ...
```

#### **Pros & Cons**
*   **Pros**:
    *   **Extremely Simple**: Can be parsed in C/C++ with `pread`/`fread` and casting. No libraries required.
    *   **Simulator-Ready**: Data segments correspond directly to `data_wdata` bursts. Config segments correspond to `cmd_wdata` writes.
    *   **Compact**: Minimal overhead.
*   **Cons**:
    *   **Rigid**: Extensions require changing the struct definition.
    *   **Manual Debugging**: Not compatible with standard tools like `objdump` or `nm`.
    *   **Endianness**: Must define endianness (usually Little Endian) strictly.

---

### **Format B: The ELF-Based Container (Standard)**
**Concept**: Treat the Cluster setup as a standard executable program using the ELF (Executable and Linkable Format) standard.

#### **Structure**
*   **ELF Heade**r: `e_machine` = `EM_HYBRIDACC` (Custom ID).
*   **Program Headers (`Elf64_Phdr`)**:
    *   `PT_LOAD`: Segments to be loaded into SPM (Weights, Instructions). `p_paddr` holds the SPM address.
*   **Sections (`Elf64_Shdr`)**:
    *   `.text`: PE instructions (mapped to SPM regions).
    *   `.data`: Weights/Input features.
    *   `.mmio`: A special section containing key-value pairs for register configuration (HDDU setup).
    *   `.symtab`: Symbol table for debugging (function names, tensor names).

#### **Example (readelf output)**
```text
ELF Header:
  Magic:   7f 45 4c 46 ...
  Type:    EXEC (Executable file)
  Entry point address: 0x0

Program Headers:
  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align
  LOAD           0x1000   0x0000000000002000 0x0000000000002000 0x1000   0x1000   R   0x1000
  LOAD           0x2000   0x0000000000003000 0x0000000000003000 0x0800   0x0800   RW  0x1000

Section Headers:
  [Nr] Name              Type             Address           Offset
  [ 1] .text             PROGBITS         0000000000002000  00001000
  [ 2] .noc_config       PROGBITS         0000000000000000  00003000
```

#### **Pros & Cons**
*   **Pros**:
    *   **Tool Compatibility**: Works with standard GNU Binutils (`readelf`, `objdump`, `nm`, `gdb`).
    *   **Standard Linking**: Can reuse standard linkers `ld` (with linker scripts) to handle address resolution.
    *   **Metadata**: Native support for symbol tables and debug info (DWARF).
*   **Cons**:
    *   **Complexity**: Requires a full ELF parser in your `Driver` (e.g., `libelf` or extensive boilerplate code).
    *   **Overhead**: Header overhead is significant for very small kernels.
    *   **abstraction Mismatch**: ELF is designed for CPU memory spaces. Mapping "HDDU Registers" to ELF sections feels hacky.

---

### **Format C: The "Filesystem" Package (Zip/Tar + Manifest)**
**Concept**: A folder (zipped) containing raw binary blobs and a descriptive manifest (JSON/YAML).

#### **Structure**
Archive `workload.hap`:
1.  `manifest.json`: The "Superblock" describing how to load the system.
2.  `kernel_0.bin`: Binary instruction for PE 0.
3.  `weights.dat`: Binary weight tensor.
4.  `input.dat`: Input feature map.

#### **Example (`manifest.json`)**
```json
{
  "name": "ResNet50_Block1",
  "version": "1.2",
  "resources": {
    "pe_count": 16,
    "spm_size": 1048576
  },
  "loading_plan": [
    {
      "file": "weights.dat",
      "target": "SPM",
      "address": "0x2000",
      "size": 1024
    },
    {
      "file": "kernel_0.bin",
      "target": "SPM",
      "address": "0x4000",
      "target_pes": [0, 1, 2, 3] // Hint to driver to configure HDDU to distribute this
    }
  ],
  "configuration": {
    "hddu_base": "0x1000",
    "noc_routing": "mesh_xy"
  }
}
```

#### **Pros & Cons**
*   **Pros**:
    *   **Human Readable**: Easiest to debug and modify by hand (just unzip, edit JSON, zip).
    *   **Flexible**: Easy to add new fields (e.g., "power_mode", "expected_cycles").
    *   **Modular**: Can reuse binary blobs across different packages easily.
*   **Cons**:
    *   **Parsing Overhead**: Requires JSON parser + Zip extraction library in the C++ Simulator/Driver.
    *   **Performance**: Slower to load than a flat binary copy.
    *   **Not a "File"**: It's a directory structure, making it slightly more complex to handle in simple makefiles.

---

## **Recommendation**

For the current stage (Simulator & Cluster verification):

**Use Format A (HAP - Flat Binary)** or a simplified variant of it.
1.  **Why?** You avoid external dependencies (JSON libraries, LibELF) in your SystemC simulator.
2.  **Workflow**:
    - The `Linker` is a Python script that packs binary files and generates the header.
    - The `Simulator` just `mmap`s the file (or `fread`s it) and iterates through the segment headers, blasting data to the `SPM` or `MMIO` ports.
3.  **Future Proofing**: Store metadata in the "Type=2 (INFO)" segment if you need JSON-like flexibility later, without breaking the binary loading mechanism.
