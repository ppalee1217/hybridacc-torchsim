#!/bin/bash

# Check if SystemC is installed
if [ -z "$SYSTEMC_HOME" ]; then
    echo "Error: SYSTEMC_HOME is not set. Please set it to your SystemC installation directory."
    exit 1
fi

# Add SystemC include and library paths
export CPLUS_INCLUDE_PATH=$SYSTEMC_HOME/include:$CPLUS_INCLUDE_PATH
export LIBRARY_PATH=$SYSTEMC_HOME/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=$SYSTEMC_HOME/lib:$LD_LIBRARY_PATH

echo "SystemC environment has been set up successfully."