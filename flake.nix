{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      treefmt-nix,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        treefmtEval = treefmt-nix.lib.evalModule pkgs {
          projectRootFile = "flake.nix";
          programs.nixfmt.enable = true;
          programs.clang-format.enable = true;
          programs.ruff-format.enable = true;
        };
      in
      {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            clang-tools
            esphome
            nixfmt
            ruff
          ];
        };

        formatter = treefmtEval.config.build.wrapper;

        checks.formatting = treefmtEval.config.build.check self;

        # esphome compile requires network access, so use
        # `nix run .#check` from the project root.
        packages.check = pkgs.writeShellApplication {
          name = "check";
          runtimeInputs = with pkgs; [ esphome ];
          text =
            let
              dummy_secrets = builtins.toFile "secrets.yaml" ''
                encryption_key: "qBeQ7x+DSLlOjQOCUkm513o9DTEugk6Ze/JHkMuK4DE="
                mgmt_pass: "test_password"
                wifi_ssid: "TestNetwork"
                wifi_password: "test_wifi_password"
              '';
            in
            ''
              set -euo pipefail
              if [ ! -f secrets.yaml ]; then
                echo "Creating dummy secrets.yaml for build testing..."
                cp ${dummy_secrets} secrets.yaml
              fi
              echo "=== Checking formatting ==="
              nix fmt -- --fail-on-change
              echo "=== Compiling demo.yaml ==="
              esphome compile demo.yaml
              echo "=== All checks passed ==="
            '';
        };
      }
    );
}
