# Libraries that the host's NVIDIA driver needs, other than glibc and its own
# libraries.
#
# NVIDIA's libraries don't have a RUNPATH, so when they're loaded into a
# nix-built program, these wouldn't be found. AppRun puts them in the ld.so
# cache of the bundled glibc(s), which is only consulted after RUNPATH, so they
# won't override the bundled program's own copies of these libraries.
#
# This list comes from the NEEDED entries of the libraries in NVIDIA's driver
# package (e.g. libnvidia-egl-wayland needs libwayland-server).
{ lib
, runCommand
, libdrm
, libgbm
, libglvnd
, wayland
, stdenv

  # newer nixpkgs deprecate the xorg set in favour of these top-level packages
, xorg ? { }
, libx11 ? xorg.libX11
, libxext ? xorg.libXext
, libxcb ? xorg.libxcb
}:

let
  libs = {
    "libX11.so.6" = libx11;
    "libX11-xcb.so.1" = libx11;
    "libXext.so.6" = libxext;
    "libxcb.so.1" = libxcb;
    "libxcb-dri3.so.0" = libxcb;
    "libxcb-glx.so.0" = libxcb;
    "libxcb-present.so.0" = libxcb;
    "libdrm.so.2" = libdrm;
    "libgbm.so.1" = libgbm;
    "libwayland-client.so.0" = wayland;
    "libwayland-server.so.0" = wayland;
    "libGLdispatch.so.0" = libglvnd;
    "libgcc_s.so.1" = stdenv.cc.cc;
  };
in
runCommand "nix-appimage-host-driver-deps" { } ''
  mkdir -p $out/lib
  ${lib.concatStrings (lib.mapAttrsToList (soname: pkg: ''
    ln -s ${lib.getLib pkg}/lib/${soname} $out/lib/${soname}
    test -e $out/lib/${soname}
  '') libs)}
''
