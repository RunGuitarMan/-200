#!/bin/bash

PID_FILE="server.pid"

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    echo "Stopping server (PID: $PID)..."
    kill "$PID" 2>/dev/null
    # Wait for process to exit
    for i in $(seq 1 30); do
        if ! kill -0 "$PID" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    rm -f "$PID_FILE"
    echo "Server stopped."
else
    echo "No PID file found. Trying pkill -x server..."
    pkill -x server
    if [ $? -eq 0 ]; then
        echo "Server processes stopped."
    else
        echo "No server processes found."
    fi
fi
