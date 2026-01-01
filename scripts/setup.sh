#!/bin/bash

# Exit on error
set -e

# Project Root
PROJECT_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
LIBS_DIR="$PROJECT_ROOT/libs"
SYSTEMC_VERSION="2.3.3"
SYSTEMC_DIR="$LIBS_DIR/systemc"

# 1. Setup SystemC
echo "Setting up SystemC..."

if [ -d "$SYSTEMC_DIR" ]; then
    echo "SystemC directory already exists at $SYSTEMC_DIR"
else
    echo "SystemC not found. Creating libs directory..."
    mkdir -p "$LIBS_DIR"
    
    cd "$LIBS_DIR"
    
    echo "Downloading SystemC $SYSTEMC_VERSION..."
    # Check for wget or curl
    if command -v wget &> /dev/null; then
        wget -c "https://www.accellera.org/images/downloads/standards/systemc/systemc-$SYSTEMC_VERSION.tar.gz" -O systemc.tar.gz
    elif command -v curl &> /dev/null; then
        curl -L "https://www.accellera.org/images/downloads/standards/systemc/systemc-$SYSTEMC_VERSION.tar.gz" -o systemc.tar.gz
    else
        echo "Error: Neither wget nor curl found. Please install one of them."
        exit 1
    fi
    
    echo "Extracting SystemC..."
    tar -xzf systemc.tar.gz
    
    # The tarball usually extracts to systemc-2.3.3
    if [ -d "systemc-$SYSTEMC_VERSION" ]; then
        mv "systemc-$SYSTEMC_VERSION" systemc
    fi
    
    rm systemc.tar.gz
    echo "SystemC setup complete."
fi

# 2. Setup uv env
echo "Setting up uv environment..."

# Check if uv is installed
if ! command -v uv &> /dev/null; then
    echo "uv not found. Installing uv..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    
    # Add uv to PATH for the current session if it was just installed
    if [ -f "$HOME/.cargo/env" ]; then
        source "$HOME/.cargo/env"
    elif [ -f "$HOME/.local/bin/env" ]; then
         export PATH="$HOME/.local/bin:$PATH"
    fi
fi

# Create virtual environment and install dependencies
# Assuming the python project is in python/ directory
if [ -d "$PROJECT_ROOT/python" ]; then
    echo "Setting up Python environment in $PROJECT_ROOT..."
    cd "$PROJECT_ROOT"
    
    # Create venv if it doesn't exist
    if [ ! -d ".venv" ]; then
        uv venv
    fi
    
    # Install dependencies
    source .venv/bin/activate
    
    echo "Installing dependencies from python/pyproject.toml..."
    cd python
    uv pip install -e .
    
    echo "uv environment setup complete."
else
    echo "Warning: python directory not found. Skipping python setup."
fi

echo "Setup script finished successfully."
