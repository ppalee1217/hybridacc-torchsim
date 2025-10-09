#!/bin/bash

# Check if SystemC is set up
if [ ! -d "$SYSTEMC_HOME" ]; then
    echo "Error: SYSTEMC_HOME is not set. Please set it to your SystemC installation directory."
    exit 1
fi

# Compile the project
echo "Compiling the project..."
mkdir -p build
cd build
cmake .. -DSYSTEMC_HOME=$SYSTEMC_HOME
make

# Run the simulation
echo "Running the simulation..."
./top

# Check the result
if [ $? -eq 0 ]; then
    echo "Simulation completed successfully."
else
    echo "Simulation failed."
    exit 1
fi