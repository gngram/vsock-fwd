#!/usr/bin/env bash
set -e

# Removed sudo check

WORKSPACE_DIR="$(pwd)"
echo "=> 1. Building the kernel module..."
if [ -n "$SUDO_USER" ]; then
    sudo -u "$SUDO_USER" make
else
    make
fi

echo "=> 2. Setting up test environment..."
rm -rf "$WORKSPACE_DIR/test-result"
mkdir -p "$WORKSPACE_DIR/test-result/logs"
cd "$WORKSPACE_DIR"

echo "=> 3. Loading the kernel module to forward port 10000 to Admin VM (CID 4)..."
sudo rmmod vsock_fwd 2>/dev/null || true
sudo insmod vsock_fwd.ko dest_cid=4 port_start=10000 port_end=10000 port_offset=0
echo "Please ensure the module is loaded manually (e.g. using 'sudo insmod vsock_fwd.ko dest_cid=4 port_start=10000 port_end=10000 port_offset=0')"

echo "=> 4. Building NixOS VMs..."
if [ -n "$SUDO_USER" ]; then
    sudo -u "$SUDO_USER" nix-build '<nixpkgs/nixos>' -A vm -I nixos-config=nix/checks/admin.nix -o target/result-admin
    sudo -u "$SUDO_USER" nix-build '<nixpkgs/nixos>' -A vm -I nixos-config=nix/checks/guest.nix -o target/result-guest
else
    nix-build '<nixpkgs/nixos>' -A vm -I nixos-config=nix/checks/admin.nix -o target/result-admin
    nix-build '<nixpkgs/nixos>' -A vm -I nixos-config=nix/checks/guest.nix -o target/result-guest
fi

echo "=> 5. Starting host listener for port 10000..."
python3 -c '
import socket, sys
s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
s.bind((socket.VMADDR_CID_ANY, 10000))
s.listen(1)
s.settimeout(20)
try:
    conn, addr = s.accept()
    data = conn.recv(1024)
    if b"Hello from guest" in data:
        with open("test-result/host-10000-received", "w") as f:
            f.write("OK")
    conn.sendall(b"Hello from host")
    conn.close()
except socket.timeout:
    with open("test-result/host-10000-timeout", "w") as f:
        f.write("OK")
' &
HOST_10000_PID=$!

echo "=> 6. Starting host listener for port 10001..."
python3 -c '
import socket, sys
s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
s.bind((socket.VMADDR_CID_ANY, 10001))
s.listen(1)
s.settimeout(20)
try:
    conn, addr = s.accept()
    data = conn.recv(1024)
    if b"Hello from guest" in data:
        with open("test-result/host-10001-received", "w") as f:
            f.write("OK")
    conn.sendall(b"Hello from host")
    conn.close()
except socket.timeout:
    with open("test-result/host-10001-timeout", "w") as f:
        f.write("OK")
' &
HOST_10001_PID=$!

echo "=> 7. Booting Admin VM (CID 4)..."
QEMU_OPTS="-m 512" QEMU_NET_OPTS="hostfwd=tcp::10022-:22" ./target/result-admin/bin/run-admin-vm >$WORKSPACE_DIR/test-result/logs/admin-vm.log 2>&1 &
ADMIN_VM_PID=$!

echo "=> Waiting for admin VM to be ready..."
for i in {1..20}; do
    if [ -f "$WORKSPACE_DIR/test-result/admin-10000-ready" ]; then
        break
    fi
    sleep 2
done

echo "=> 8. Booting Guest VM (CID 3)..."
QEMU_OPTS="-m 512" QEMU_NET_OPTS="hostfwd=tcp::10023-:22" ./target/result-guest/bin/run-guest-vm >$WORKSPACE_DIR/test-result/logs/guest-vm.log 2>&1 &
GUEST_VM_PID=$!

echo "=> Waiting for test to complete (20s)..."
wait $HOST_10000_PID || true
wait $HOST_10001_PID || true
sleep 5 # give guest/admin some time

echo "=> 9. Stopping VMs..."
kill $ADMIN_VM_PID || true
kill $GUEST_VM_PID || true

echo "=> 10. Unloading the kernel module..."
# rmmod vsock_fwd || true
echo "Please manually unload the module when done (e.g. using 'sudo rmmod vsock_fwd')"

echo "======================================================"
echo "                   TEST RESULTS                       "
echo "======================================================"

FAIL=0

if [ -f "test-result/host-10001-received" ]; then
    echo "[PASS] Data sent by guest at port 10001 is received in host"
else
    echo "[FAIL] Data sent by guest at port 10001 was NOT received in host"
    FAIL=1
fi

if [ -f "test-result/host-10000-timeout" ] && [ ! -f "test-result/host-10000-received" ]; then
    echo "[PASS] No data received at host port 10000"
else
    echo "[FAIL] Data was received at host port 10000 or no timeout occurred"
    FAIL=1
fi

if [ -f "test-result/admin-10000-received" ]; then
    echo "[PASS] Data sent on 10000 port is received by admin"
else
    echo "[FAIL] Data sent on 10000 port was NOT received by admin"
    FAIL=1
fi

if [ $FAIL -eq 0 ]; then
    echo "SUCCESS" > test-result/result-summary
    echo "Test completed successfully!"
    chmod -R 777 $WORKSPACE_DIR/test-result
    sudo rmmod vsock_fwd 2>/dev/null || true
    exit 0
else
    echo "FAILURE" > test-result/result-summary
    echo "Test failed!"
    chmod -R 777 $WORKSPACE_DIR/test-result
    sudo rmmod vsock_fwd 2>/dev/null || true
    exit 1
fi
