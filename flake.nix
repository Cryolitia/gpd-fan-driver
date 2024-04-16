{
  description = "Nix Flake";

  nixConfig = {
    experimental-features = [ "nix-command" "flakes" ];
  };

  inputs =
    {
      nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    };

  outputs =
    inputs:
    let
      system = "x86_64-linux";
      pkgs = import inputs.nixpkgs {
        inherit system;
      };
    in
    {
      devShells."${system}".default = pkgs.mkShell {
        buildInputs = with pkgs; [
          linuxPackages_zen.kernel.dev
        ];

        shellHook = ''
          export KERNEL_SRC=${pkgs.linuxPackages_zen.kernel.dev}/lib/modules/${pkgs.linuxPackages_zen.kernel.modDirVersion}/build
        '';
      };
    };
}
