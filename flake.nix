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
        config.allowUnfree = true;
      };
      modulePackage = ({ lib
                       , stdenv
                       , kernel
                       }:

        stdenv.mkDerivation {
          pname = "gpd-fan-driver";
          version = "0-unstable";

          src = lib.cleanSource ./.;

          hardeningDisable = [ "pic" ];

          nativeBuildInputs = kernel.moduleBuildDependencies;

          makeFlags = [
            "KERNEL_SRC=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build"
          ];

          installPhase = ''
            runHook preInstall

            install *.ko -Dm444 -t $out/lib/modules/${kernel.modDirVersion}/kernel/drivers/gpdfan

            runHook postInstall
          '';

          meta = with lib; {
            homepage = "https://github.com/Cryolitia/gpd-fan-driver/";
            description = "A kernel driver for the GPD devices fan";
            license = with licenses; [ gpl2Plus ];
            maintainers = with maintainers; [ Cryolitia ];
            platforms = [ "x86_64-linux" ];
          };
        });
    in
    {
      inherit modulePackage;
      
      devShells."${system}".default = pkgs.mkShell {
        buildInputs = with pkgs; [
          linuxPackages_zen.kernel.dev
          flex
          bison
          jetbrains.clion
          python3
        ];

        shellHook = ''
          export KERNEL_SRC=${pkgs.linuxPackages_zen.kernel.dev}/lib/modules/${pkgs.linuxPackages_zen.kernel.modDirVersion}/build
          zsh
        '';
      };

      packages.${system}.default = pkgs.linuxPackages.callPackage modulePackage { };

      nixosModules.default = ({ config, lib, ... }:

        with lib;

        let

          gpd-fan = config.boot.kernelPackages.callPackage modulePackage { };

        in
        {

          meta.maintainers = [ maintainers.Cryolitia ];

          ###### interface

          options = {
            hardware.gpd-fan.enable = mkEnableOption "Enable GPD deices fan driver";
          };

          ###### implementation

          config = mkIf config.hardware.gpd-fan.enable {
            boot.extraModulePackages = [ gpd-fan ];
            boot.kernelModules = [ "gpd_fan" ];
          };
        });
    };
}
