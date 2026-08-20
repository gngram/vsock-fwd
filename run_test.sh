#!/usr/bin/env bash

# Compile the module
echo "[*] Compiling the kernel module..."
nix-shell --run "make"

# Check if loaded
if [ ! -d "/proc/vsock_fwd" ]; then
    echo "=========================================================="
    echo "IMPORTANT: The kernel module is not loaded yet."
    echo "Due to shell permission constraints (no new privileges),"
    echo "please run the following command in your host terminal:"
    echo ""
    echo "    sudo insmod $(pwd)/vsock_fwd.ko vmx_cid=1 port_start=8080 port_end=8080 port_offset=1000"
    echo ""
    echo "=========================================================="
    read -p "Press ENTER once you have run the command above..."
fi

if [ ! -d "/proc/vsock_fwd" ]; then
    echo "Error: /proc/vsock_fwd still not found. Exiting."
    exit 1
fi

echo "[+] Module loaded. Current config:"
cat /proc/vsock_fwd/config

echo "[*] Starting Listener on port 9080 (simulating VM-X)..."
nix-shell -p python3 --run "python3 vsock_listener.py --port 9080" &
LISTENER_PID=$!
sleep 2

echo "[*] Running Client connecting to CID 2, port 8080 (simulating Guest connection)..."
nix-shell -p python3 --run "python3 vsock_client.py --cid 2 --port 8080"

# Kill listener just in case
kill $LISTENER_PID 2>/dev/null

echo "[+] Active sessions recorded during test:"
cat /proc/vsock_fwd/sessions

echo "=========================================================="
echo "To clean up and unload the module, please run on host:"
echo ""
echo "    sudo rmmod vsock_fwd"
echo "=========================================================="
