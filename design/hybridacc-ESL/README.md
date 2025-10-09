# hybridacc ESL Project

This project implements a hybrid accelerator design using SystemC for hardware modeling. The goal is to provide a flexible and efficient framework for developing and simulating hardware accelerators.

## Project Structure

- **CMakeLists.txt**: Configuration file for CMake, defining build rules and dependencies.
- **README.md**: Documentation for setting up and using the hybrid accelerator design.
- **.vscode/**: Contains settings and tasks for Visual Studio Code.
- **include/hybridacc/**: Header files defining the accelerator and interconnect classes.
- **src/**: Source files implementing the functionality of the accelerator and interconnect.
- **tb/**: Testbench files for verifying the design.
- **config/**: Configuration files for parameters and platform settings.
- **scripts/**: Shell scripts for setting up the SystemC environment and running simulations.
- **docs/**: Documentation files describing the architecture and design philosophy.
- **cmake/**: CMake scripts for finding SystemC and defining toolchains.

## Getting Started

1. **Clone the repository**:
   ```
   git clone <repository-url>
   cd hybridacc-ESL
   ```

2. **Set up the SystemC environment**:
   Run the setup script:
   ```
   ./scripts/setup_systemc.sh
   ```

3. **Build the project**:
   Use CMake to configure and build the project:
   ```
   mkdir build
   cd build
   cmake ..
   make
   ```

4. **Run the simulation**:
   Execute the simulation script:
   ```
   ./scripts/run_sim.sh
   ```

## Documentation

Refer to the `docs/architecture.md` file for detailed information about the architecture and design principles of the hybrid accelerator.

## Contributing

Contributions are welcome! Please submit a pull request or open an issue for any enhancements or bug fixes.