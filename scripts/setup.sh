#!/bin/bash

# Exit on error
set -e

# Project Root
PROJECT_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
PACKAGES_DIR="$PROJECT_ROOT/packages"
LIBS_DIR="$PROJECT_ROOT/libs"
SYSTEMC_VERSION="2.3.3"

SYSTEMC_PACKAGE="$PACKAGES_DIR/systemc-$SYSTEMC_VERSION"
SYSTEMC_LIB="$LIBS_DIR/systemc-$SYSTEMC_VERSION"

# 1. Setup SystemC
echo "=== 1 ==="
echo "Setting up SystemC..."

if [ -d "$SYSTEMC_PACKAGE" ]; then
    echo "SystemC directory already exists at $SYSTEMC_PACKAGE. Skipping download and extraction."
else
    echo "SystemC not found. Creating libs directory..."
    mkdir -p "$PACKAGES_DIR"

    cd "$PACKAGES_DIR"

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

    # The tarball usually extracts to systemc-2.3.3
    echo "Extracting SystemC..."
    tar -xzf systemc.tar.gz

    # Clean up tarball
    rm systemc.tar.gz
fi

if [ -d "$SYSTEMC_LIB" ]; then
    echo "SystemC already installed at $SYSTEMC_LIB"
else
    echo "Installing SystemC..."

    mkdir -p "$SYSTEMC_PACKAGE"/build
    cd "$SYSTEMC_PACKAGE"/build
    cmake "$SYSTEMC_PACKAGE" -DCMAKE_INSTALL_PREFIX="$SYSTEMC_LIB" -DCMAKE_CXX_STANDARD=17
    make -j$(nproc)
    make install
    rm -rf "$SYSTEMC_PACKAGE"/build

    echo "SystemC setup complete."
fi

# 2. Set SYSTEMC_LIB environment variable
echo "=== 2 ==="
echo "Setting SYSTEMC_LIB environment variable..."
if grep -q "export SYSTEMC_LIB=" ~/.bashrc; then
    echo "SYSTEMC_LIB already set in ~/.bashrc. Updating value..."
    sed -i "s|export SYSTEMC_LIB=.*|export SYSTEMC_LIB=$SYSTEMC_LIB|" ~/.bashrc
else
    echo "Adding SYSTEMC_LIB to ~/.bashrc..."
    echo "export SYSTEMC_LIB=$SYSTEMC_LIB" >> ~/.bashrc
fi

# 3. Setup uv env
echo "=== 3 ==="
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
    else
        echo "Virtual environment already exists."
    fi

    # Activate venv
    source .venv/bin/activate

    # Prefer modern pyproject at repo root; fall back to python/setup.py editable install
    if [ -f "$PROJECT_ROOT/pyproject.toml" ]; then
        echo "Installing python package from project root (pyproject.toml)..."
        uv pip install -e .
    else
        echo "Warning: no pyproject.toml or python/setup.py found; skipping python install."
    fi

    echo "uv environment setup complete."
else
    echo "Warning: python directory not found. Skipping python setup."
fi

echo "Setup script finished successfully."
echo "Please run 'source ~/.bashrc' to apply environment variable changes."
