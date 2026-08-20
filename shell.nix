{ pkgs ? import <nixpkgs> {} }:

let
  kernel = pkgs.linuxPackages.kernel;
in
pkgs.mkShell {
  nativeBuildInputs = [
    pkgs.gnumake
    pkgs.gcc
  ];
  
  shellHook = ''
    export KDIR="${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    echo "=========================================================="
    echo "      VSOCK Redirect Proxy Kernel Module Dev Shell"
    echo "=========================================================="
    echo "Kernel build environment loaded."
    echo "KDIR is set to: $KDIR"
    echo ""
    echo "1. To build the kernel module:"
    echo "   make"
    echo ""
    echo "2. To load the kernel module on your host:"
    echo "   sudo insmod vsock_fwd.ko vmx_cid=1 port_start=8080 port_end=8080 port_offset=1000"
    echo ""
    echo "3. To test the redirection loop via loopback (in separate terminals):"
    echo "   - Run the listener (simulating VM-X):"
    echo "     nix-shell -p python3 --run \"python3 vsock_listener.py --port 9080\""
    echo "   - Run the client (simulating Guest connecting to host):"
    echo "     nix-shell -p python3 --run \"python3 vsock_client.py --cid 2 --port 8080\""
    echo ""
    echo "4. To monitor active configuration and sessions:"
    echo "   cat /proc/vsock_fwd/config"
    echo "   cat /proc/vsock_fwd/sessions"
    echo ""
    echo "5. To unload the module when finished:"
    echo "   sudo rmmod vsock_fwd"
    echo "=========================================================="
  '';
}
