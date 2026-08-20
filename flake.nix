{
  description = "VSOCK Forwarding Kernel Module";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    treefmt-nix.url = "github:numtide/treefmt-nix";
    treefmt-nix.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = {
    self,
    nixpkgs,
    treefmt-nix, # Added to outputs destructured argument list
  }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs {inherit system;};

    # Evaluate multi-language treefmt rules for this specific target system
    treefmtEval = treefmt-nix.lib.evalModule pkgs {
      projectRootFile = "flake.nix";

      # Enable requested code formatter programs
      programs.alejandra.enable = true;
      programs.black.enable = true;
      programs.clang-format.enable = true;
      programs.shfmt = {
        enable = true;
        indent_size = 4;
      };
    };
  in {
    # Binds configuration wrapper dynamically to standard `nix fmt` terminal call
    formatter.${system} = treefmtEval.config.build.wrapper;

    # Pass the treefmt wrapper downstream into your development environment if required
    devShells.${system}.default = import ./nix/develop.nix {
      inherit pkgs;
      # You can reference treefmtEval.config.build.wrapper here if you'd like to append it to your devShell path inside develop.nix
    };
  };
}
