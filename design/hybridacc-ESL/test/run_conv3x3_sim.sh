#!/bin/bash

# Conv3x3 Processing Pass Simulation Runner Script

echo "=========================================="
echo "Conv3x3 Processing Pass Simulation"
echo "=========================================="

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if build directory exists
if [ ! -d "build" ]; then
    echo -e "${YELLOW}Build directory not found. Creating and configuring...${NC}"
    mkdir build
    cd build
    cmake .. -DENABLE_DEBUG_UTILS=ON
    if [ $? -ne 0 ]; then
        echo -e "${RED}CMake configuration failed!${NC}"
        exit 1
    fi
    cd ..
fi

# Build the project
echo -e "\n${YELLOW}Building test_pe_sim...${NC}"
cd build
make test_pe_sim -j$(nproc)

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}Build successful!${NC}"

# Run the simulation
echo -e "\n${YELLOW}Running Conv3x3 simulation...${NC}"
./test_pe_sim

# Check exit code
if [ $? -eq 0 ]; then
    echo -e "\n${GREEN}Simulation completed successfully!${NC}"
else
    echo -e "\n${RED}Simulation failed!${NC}"
    exit 1
fi

echo "=========================================="
