# HybridAcc ESL Simulator

This project contains the Electronic System Level (ESL) simulator for the HybridAcc heterogeneous accelerator. It is implemented using SystemC.

## Project Structure

```
hybridacc-ESL/
├── simulator/
│   ├── include/        # Header files (Core logic)
│   │   ├── Cluster/    # HDDU and memory components
│   │   ├── NoC/        # Network-on-Chip components (Router, MBUS)
│   │   ├── PE/         # Process Element components (Stages, Units)
│   │   └── ...
│   └── src/            # Source files (Implementation)
├── test/               # Testbenches and Simulation Entry Points
│   ├── test_noc_sim.cpp # Top-level NoC simulation
│   ├── test_pe_sim.cpp  # PE-level simulation
│   └── ...
├── doc/                # Documentation
└── CMakeLists.txt      # Build configuration
```

## Prerequisites

*   **CMake** (3.10 or higher)
*   **C++ Compiler** (supporting C++17)
*   **SystemC** (Must be installed and configured)

## Building the Simulator

1.  Create a build directory:
    ```bash
    mkdir build
    cd build
    ```

2.  Configure the project using CMake:
    ```bash
    cmake ..
    ```
    *Note: You may need to specify the SystemC path if it's not in a standard location, e.g., `cmake -DCMAKE_PREFIX_PATH=/path/to/systemc ..`*

3.  Build the executables:
    ```bash
    make
    ```

## Running Simulations

After building, the executables will be located in the `build/test/` directory (or similar, depending on CMake configuration).

### NoC System Simulation
To run the full system simulation (NoC + PEs):
```bash
./test/test_noc_sim
```
This testbench loads configuration and data files (weights, inputs, programs) and simulates the execution of the accelerator.

### PE Unit Simulation
To run the Process Element simulation in isolation:
```bash
./test/test_pe_sim
```

## Documentation

Detailed documentation for the components can be found in the `doc/` directory:
*   [System Architecture](doc/HybridAcc.md)
*   [Network-on-Chip](doc/NetworkOnChip.md)
*   [Process Element](doc/ProcessElement.md)
*   [Hybrid Data Deliver Unit](doc/HybridDataDeliverUnit.md)
