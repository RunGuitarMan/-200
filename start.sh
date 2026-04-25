#!/bin/bash

# Build using Makefile
echo "Building server..."
make -s

# Increase file descriptor limit
echo "Setting system limits..."
ulimit -n 12288

# Detect CPU cores
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
echo "Detected $CORES cores"

# Start server with multi-worker mode
echo "Starting server with $CORES workers..."
./server -w "$CORES"
