{
  config,
  pkgs,
  ...
}: {
  # Set a hostname for the VM
  networking.hostName = "admin";
  nix.settings.experimental-features = ["nix-command" "flakes"];
  nix.nixPath = ["nixpkgs=${pkgs.path}"];

  # --- User Accounts ---
  users.users.demo = {
    isNormalUser = true;
    initialPassword = "nixos";
    extraGroups = ["wheel"];
  };

  time.timeZone = "UTC";

  # --- Shared Workspace & VSOCK Configuration ---
  virtualisation.vmVariant = {
    virtualisation.sharedDirectories.workspace = {
      source = toString ./../..;
      target = "/workspace";
    };

    virtualisation.qemu.options = [
      "-device vhost-vsock-pci,guest-cid=4"
    ];
  };

  environment.systemPackages = with pkgs; [ python3 ];

  systemd.services.admin-listener-10000 = {
    description = "Admin listener on port 10000";
    wantedBy = ["multi-user.target"];
    serviceConfig = {
      Type = "simple";
      ExecStart = pkgs.writeShellScript "listen-10000" ''
        ${pkgs.python3}/bin/python3 -c '
import socket
s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
s.bind((socket.VMADDR_CID_ANY, 10000))
s.listen(1)
print("Admin listening on port 10000")
with open("/workspace/test-result/admin-10000-ready", "w") as f:
    f.write("OK")
conn, addr = s.accept()
print("Accepted connection from", addr)
data = conn.recv(1024)
print("Received:", data.decode())
if b"Hello from guest" in data:
    with open("/workspace/test-result/admin-10000-received", "w") as f:
        f.write("OK")
conn.sendall(b"Hello from admin")
conn.close()
'
      '';
    };
  };

  system.stateVersion = "26.05";
}
