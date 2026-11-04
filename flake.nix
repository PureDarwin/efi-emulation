{
  description = "efi-emulation: boot an EFI application as a Linux or multiboot2 kernel";

  # the toolchain and gnu-efi xnu-loader is known to boot with, others broke the shim
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/418468ac9527e799809c900eda37cbff999199b6";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      cross = pkgs.pkgsCross.aarch64-multiplatform;

      # one cmake build per target, gnu-efi comes from that target's package set
      build = { name, target, toolchain, cmakeFlags ? [ ] }: target.stdenv.mkDerivation {
        pname = name;
        version = "0.1";
        src = self;
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.nasm ];
        cmakeFlags = [
          "-DCMAKE_TOOLCHAIN_FILE=${self}/cmake/toolchain-${toolchain}.cmake"
          "-DGNU_EFI_ROOT=${target.gnu-efi}"
        ] ++ cmakeFlags;
        hardeningDisable = [ "all" ];
      };
    in
    {
      packages.${system} = {
        default = self.packages.${system}.x86_64;
        x86_64 = build { name = "efi-emulation-x86_64"; target = pkgs; toolchain = "x86_64"; };
        arm64 = build { name = "efi-emulation-arm64"; target = cross; toolchain = "aarch64"; };
        arm64-sun50i = build {
          name = "efi-emulation-arm64-sun50i";
          target = cross;
          toolchain = "aarch64";
          cmakeFlags = [ "-DEFIEMU_PLATFORM=sun50i" ];
        };
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = [ pkgs.cmake pkgs.ninja pkgs.nasm pkgs.gcc cross.buildPackages.gcc ];
        hardeningDisable = [ "all" ];
        GNU_EFI_X86_64 = "${pkgs.gnu-efi}";
        GNU_EFI_AARCH64 = "${cross.gnu-efi}";
      };
    };
}
