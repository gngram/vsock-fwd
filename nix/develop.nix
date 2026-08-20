{pkgs}:
let
  kernel = pkgs.linuxPackages.kernel;
in
pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    gnumake
    gcc
    python3
    nix
  ];

  shellHook = ''
    export KDIR="${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
    echo "=========================================================="
    echo "      VSOCK Redirect Proxy Kernel Module Dev Shell"
    echo "=========================================================="
    echo "Kernel build environment loaded."
    echo "KDIR is set to: $KDIR"
    export PS1="\[\033[1;32m\][DEVELOP]\[\033[0m\] $PS1"
  '';
}
