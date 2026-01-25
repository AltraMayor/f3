{
  description = "F3 - Fight flash fraud";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = { self, nixpkgs }: {
    packages = nixpkgs.lib.genAttrs [ "aarch64-linux" "x86_64-linux" "aarch64-darwin" ] (system:
    let
      pkgs = import nixpkgs { inherit system; };
    in
    {
      	f3 = pkgs.stdenv.mkDerivation {
        pname = "f3";
        version = builtins.head (builtins.match ''.*#define[[:space:]]+F3_STR_VERSION[[:space:]]+"([0-9.]+)".*'' (builtins.readFile (self + "/version.h")));
        src = ./.;
        nativeBuildInputs = [ pkgs.clang ];
        buildInputs = pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.systemd
            pkgs.parted
          ]
          ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [ pkgs.argp-standalone ];

        installPhase = ''
          runHook preInstall
					mkdir -p $out/bin
					make install PREFIX=$out
          make install-extra PREFIX=$out
					runHook postInstall
        '';

        meta = with pkgs.lib; {
          description = "F3 - Fight flash fraud";
          license = licenses.gpl3;
          maintainers = [ maintainers.llamato ];
          platforms = platforms.unix;
        };
      };

			devShells.${system}.default = pkgs.mkShell {
        buildInputs = with pkgs; [ 
					cmake
					gcc
					gdb
				];
      };
    });

		defaultPackage = {
			x86_64-linux = self.packages.x86_64-linux.f3;
			aarch64-linux = self.packages.aarch64-linux.f3;
			aarch64-darwin = self.packages.aarch64-darwin.f3;
		};
  };
}