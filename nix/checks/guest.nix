{
  config,
  pkgs,
  ...
}: {
  networking.hostName = "guest";
  nix.settings.experimental-features = ["nix-command" "flakes"];
  nix.nixPath = ["nixpkgs=${pkgs.path}"];

  users.users.demo = {
    isNormalUser = true;
    initialPassword = "nixos";
    extraGroups = ["wheel"];
  };

  time.timeZone = "UTC";

  virtualisation.vmVariant = {
    virtualisation.sharedDirectories.workspace = {
      source = toString ./../..;
      target = "/workspace";
    };

    virtualisation.qemu.options = [
      "-device vhost-vsock-pci,guest-cid=3"
    ];
  };

  environment.systemPackages = with pkgs; [ python3 ];

  systemd.services.guest-dialer-10000 = {
    description = "Guest dialer to port 10000";
    wantedBy = ["multi-user.target"];
    serviceConfig = {
      Type = "oneshot";
      ExecStart = pkgs.writeShellScript "dial-10000" ''
        sleep 5
        ${pkgs.python3}/bin/python3 -c '
import socket
import sys
try:
    s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    s.connect((2, 10000))
    s.sendall(b"Hello from guest port 10000")
    data = s.recv(1024)
    print("Received:", data.decode())
    with open("/workspace/test-result/guest-10000-success", "w") as f:
        f.write("OK")
except Exception as e:
    print("Error:", e)
'
      '';
    };
  };

  systemd.services.guest-dialer-10001 = {
    description = "Guest dialer to port 10001";
    wantedBy = ["multi-user.target"];
    serviceConfig = {
      Type = "oneshot";
      ExecStart = pkgs.writeShellScript "dial-10001" ''
        sleep 5
        ${pkgs.python3}/bin/python3 -c '
import socket
import sys
try:
    s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
    s.connect((2, 10001))
    s.sendall(b"Hello from guest port 10001")
    data = s.recv(1024)
    print("Received:", data.decode())
    with open("/workspace/test-result/guest-10001-success", "w") as f:
        f.write("OK")
except Exception as e:
    print("Error:", e)
'
      '';
    };
  };

  system.stateVersion = "26.05";
}
