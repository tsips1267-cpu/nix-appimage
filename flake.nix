{
  description = "A basic AppImage bundler";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    flake-compat = {
      url = "github:edolstra/flake-compat";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    # AppImages (and the namespaces AppRun uses) are Linux-only
    flake-utils.lib.eachSystem [ "x86_64-linux" "aarch64-linux" ] (system:
      let
        # regular (non-static) nixpkgs, for things that get loaded into the
        # bundled program (e.g. graphics drivers)
        pkgsDynamic = import nixpkgs { inherit system; };
        pkgs = pkgsDynamic.pkgsStatic;

        # A derivation linking to each of attrs, which also has them as attributes.
        # `nix flake check` requires packages to be derivations, but this keeps
        # e.g. packages.<system>.appimage-runtimes.<name> working.
        linkFarmWithAttrs = name: attrs: pkgsDynamic.linkFarm name attrs // attrs;
      in
      rec {
        # runtimes are an executable that mount the squashfs part of the appimage and start AppRun
        packages.appimage-runtimes = linkFarmWithAttrs "appimage-runtimes" {
          appimagecrafters = pkgs.callPackage ./runtimes/appimagecrafters { };
          appimage-type2-runtime = pkgs.callPackage ./runtimes/appimage-type2-runtime { };
        };

        # appruns contain an AppRun executable that does setup and launches entrypoint
        packages.appimage-appruns = linkFarmWithAttrs "appimage-appruns" {
          userns-chroot = pkgs.callPackage ./appruns/userns-chroot { };
        };

        # libraries that the host's NVIDIA driver needs, which get bundled along
        # with graphics drivers
        packages.appimage-host-driver-deps = pkgsDynamic.callPackage ./graphics/host-driver-deps.nix { };

        lib.mkAppImage = pkgs.callPackage ./mkAppImage.nix {
          mkappimage-runtime = packages.appimage-runtimes.appimage-type2-runtime;
          mkappimage-apprun = packages.appimage-appruns.userns-chroot;
          mkappimage-graphics-drivers = [ pkgsDynamic.mesa ];
          mkappimage-host-driver-deps = packages.appimage-host-driver-deps;
        };

        bundlers.default = drv:
          if drv.type == "app" then
            lib.mkAppImage
              {
                program = drv.program;
              }
          else if drv.type == "derivation" then
            lib.mkAppImage
              {
                program = pkgs.lib.getExe drv;
              }
          else builtins.abort "don't know how to build ${drv.type}; only know app and derivation";

        checks =
          let
            # use regular (non-static) nixpkgs
            pkgs = pkgsDynamic;
            hello-appimage = bundlers.default pkgs.hello;

            # checks that OpenGL (GLX and EGL) and Vulkan work using the bundled
            # drivers, which in the build sandbox is software rendering
            graphics-appimage = lib.mkAppImage {
              pname = "graphics-test";
              program = toString (pkgs.writeShellScript "graphics-test" ''
                set -eux
                export PATH=${pkgs.lib.makeBinPath [ pkgs.mesa-demos pkgs.vulkan-tools pkgs.gnugrep ]}
                glxinfo -B | grep "OpenGL renderer string: llvmpipe"
                eglinfo -B | grep "OpenGL core profile renderer: llvmpipe"
                vulkaninfo --summary | grep "driverName *= llvmpipe"
              '');
            };

            # checks that the bundled program keeps our uid/gid, and can create
            # user namespaces (e.g. for sandboxing)
            namespaces-appimage = lib.mkAppImage {
              pname = "namespaces-test";
              program = toString (pkgs.writeShellScript "namespaces-test" ''
                set -eux
                export PATH=${pkgs.lib.makeBinPath [ pkgs.coreutils pkgs.util-linux ]}
                [ "$(id -u):$(id -g)" = "$1" ]
                unshare --user --map-root-user true
              '');
            };
          in
          {
            hello-is-static = pkgs.runCommand "check-hello-is-static"
              {
                nativeBuildInputs = [ (pkgs.lib.getBin pkgs.stdenv.cc.libc) ];
              } ''
              (! ldd ${hello-appimage} 2>&1) | grep "not a dynamic executable"
              touch $out
            '';

            hello-has-no-graphics = pkgs.runCommand "check-hello-has-no-graphics"
              {
                nativeBuildInputs = [ pkgs.squashfsTools ];
              } ''
              offset=$(stat -L -c%s ${packages.appimage-runtimes.appimage-type2-runtime})
              unsquashfs -o "$offset" -l ${hello-appimage} > files
              grep -q squashfs-root/entrypoint files
              if grep squashfs-root/graphics files; then
                echo "hello shouldn't have graphics drivers bundled"
                exit 1
              fi
              touch $out
            '';

            namespaces-work = pkgs.runCommand "check-namespaces-work" { } ''
              HOME=$TMPDIR ${namespaces-appimage} --appimage-extract-and-run "$(id -u):$(id -g)"
              touch $out
            '';

            graphics-works = pkgs.runCommand "check-graphics-works"
              {
                # not xvfb-run, since its Xvfb doesn't support GLX
                nativeBuildInputs = [ (pkgs.xorg-server or pkgs.xorg.xorgserver) ];
              } ''
              # Xvfb needs a driver for GLX, which it also looks for in
              # /run/opengl-driver, so point it at one. We only do this for Xvfb
              # though, since the AppImage should find its own.
              LIBGL_DRIVERS_PATH=${pkgs.mesa}/lib/dri Xvfb -displayfd 3 -screen 0 640x480x24 3>display &
              for i in $(seq 100); do
                [ -s display ] && break
                sleep 0.1
              done

              DISPLAY=:$(cat display) HOME=$TMPDIR ${graphics-appimage} --appimage-extract-and-run
              kill %1
              touch $out
            '';
          };
      });
}
