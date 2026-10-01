{ lib
, buildEnv
, runCommand
, squashfsTools
, writeTextFile

  # mkappimage-specific, passed from flake.nix
, mkappimage-runtime # runtimes are an executable that mount the squashfs part of the appimage and start AppRun
, mkappimage-apprun # appruns contain an AppRun executable that does setup and launches entrypoint
, mkappimage-graphics-drivers # default graphics drivers to bundle (mesa)
, mkappimage-host-driver-deps # libraries that the host's NVIDIA driver needs, see graphics/host-driver-deps.nix
}:

# actual arguments
{ program # absolute path of executable to start

  # output name
, pname ? (lib.last (builtins.split "/" (toString program)))
, name ? "${pname}.AppImage"

  # graphics (OpenGL, EGL, Vulkan, GBM, VA-API, VDPAU) driver support
  #
  # nix-built programs look for GPU drivers in /run/opengl-driver, which only
  # exists on NixOS. To make these programs work on other distros, we can bundle
  # drivers (and use the host's NVIDIA driver if it has one), which AppRun then
  # makes available at /run/opengl-driver if the host doesn't have it.
, graphics ? "auto" # whether to bundle drivers: true, false, or "auto" to only do so if program uses OpenGL/EGL/Vulkan/GBM
, graphicsDrivers ? mkappimage-graphics-drivers # packages that make up /run/opengl-driver, like hardware.graphics.{package,extraPackages} on NixOS

  # advanced appimage configuration
, squashfsArgs ? [ ] # additional arguments to pass to mksquashfs
}:

assert lib.assertOneOf "graphics" graphics [ true false "auto" ];

let
  commonArgs = [
    "-offset $(stat -L -c%s ${lib.escapeShellArg mkappimage-runtime})" # squashfs comes after the runtime
    "-all-root" # chown to root
    # don't depend on the build machine's store, which may have hardlinked
    # identical files (auto-optimise-store) or have SELinux labels
    "-no-hardlinks"
    "-no-xattrs"
  ] ++ squashfsArgs;

  # Workaround for writeClosure bug.
  #
  # Due to a bug in Nix, writeClosure with a path *under* a nix store path (e.g.
  # /nix/store/...-hello/bin/hello) raises the error "path '$program' is not in
  # the Nix store".
  #
  # See: https://github.com/ralismark/nix-appimage/issues/16
  # See: https://github.com/NixOS/nixpkgs/issues/316652
  # See: https://github.com/NixOS/nix/pull/10549
  #
  # This should be fixed in the latest version of Nix, however version where
  # this bug is present are still common, so we work around it by using the old
  # implementation of writeReferencesToFile from
  # https://github.com/NixOS/nixpkgs/blob/e99021ff754a204e38df619ac908ac92885636a4/pkgs/build-support/trivial-builders/default.nix#L628-L640
  writeReferencesToFile = path: runCommand "runtime-deps"
    {
      exportReferencesGraph = [ "graph" path ];
    }
    ''
      touch $out
      while read path; do
        echo $path >> $out
        read dummy
        read nrRefs
        for ((i = 0; i < nrRefs; i++)); do read ref; done
      done < graph
    '';

  graphicsDriversEnv = buildEnv {
    name = "nix-appimage-graphics-drivers";
    paths = graphicsDrivers;
  };

  # Adds the graphics drivers to the image. AppRun reads graphics/ in the image
  # (staged in extras/graphics) to find out what we've bundled.
  bundleGraphics = ''
    echo "bundling graphics drivers"

    sort -u closure > closure-program
    cat ${writeReferencesToFile graphicsDriversEnv} ${writeReferencesToFile mkappimage-host-driver-deps} | sort -u > closure-graphics
    cat closure-graphics >> closure

    # static libraries aren't used at runtime, and llvm's (which mesa depends
    # on) are a few hundred MB. Only exclude them from paths that are just
    # there for the drivers, to not change what the program itself sees. This
    # uses an action rather than -e, since that would depend on whether
    # squashfsArgs has -wildcards or -regex, and without them excludes files by
    # inode (so would also exclude hardlinked copies elsewhere).
    comm -13 closure-program closure-graphics | while read -r path; do
      if [ -n "$(find "$path" -type f -name '*.a' -print -quit)" ]; then
        echo "exclude@subpathname(''${path#/}) && name(*.a) && type(f)"
      fi
    done >> actions

    mkdir extras/graphics
    ln -s ${graphicsDriversEnv} extras/graphics/drivers
    ln -s ${mkappimage-host-driver-deps} extras/graphics/host-driver-deps

    # nixpkgs' glibc reads its ld.so cache from <glibc>/etc/ld.so.cache instead
    # of /etc/ld.so.cache. Add empty ones for AppRun to mount over.
    touch extras/graphics/ld.so.cache-targets
    sort -u closure | while read -r path; do
      for ldso in "$path"/lib/ld-linux*.so.*; do
        if [ -e "$ldso" ] && [ ! -e "$path/etc/ld.so.cache" ] && grep -qF "$path/etc/ld.so.cache" "$ldso"; then
          echo "$path/etc/ld.so.cache" >> extras/graphics/ld.so.cache-targets
          if [ ! -e "$path/etc" ]; then
            echo "''${path#/}/etc d 555 0 0" >> pseudo
          fi
          echo "''${path#/}/etc/ld.so.cache f 444 0 0 true" >> pseudo
          break
        fi
      done
    done
  '';

  # Whether the program uses graphics drivers, by checking for the libraries
  # that load them: libglvnd's libGL/libEGL/etc, vulkan-loader's libvulkan,
  # libgbm, libva and libvdpau.
  detectGraphics = ''
    usesGraphics=
    while read -r path; do
      for lib in libGL.so.1 libGLX.so.0 libEGL.so.1 libOpenGL.so.0 libGLESv2.so.2 libvulkan.so.1 libgbm.so.1 libva.so.2 libvdpau.so.1; do
        if [ -e "$path/lib/$lib" ]; then
          echo "program uses $path/lib/$lib"
          usesGraphics=1
          break 2
        fi
      done
    done < closure
  '';
in
runCommand name
{
  nativeBuildInputs = [
    squashfsTools
  ];
} ''
  if ! test -f ${lib.escapeShellArg program} -a -x ${lib.escapeShellArg program}; then
    echo "entrypoint '${program}' is not an executable file"
    exit 1
  fi

  ${./extra-files.sh} ${lib.escapeShellArg program}

  # the store paths to include
  cat ${writeReferencesToFile program} > closure
  # additional mksquashfs pseudo file definitions and actions
  touch pseudo actions

  ${if graphics == "auto" then ''
    ${detectGraphics}
    if [ -n "$usesGraphics" ]; then
      ${bundleGraphics}
    else
      echo "program doesn't seem to use graphics drivers, not bundling them"
    fi
  '' else lib.optionalString graphics bundleGraphics}

  mksquashfs ${builtins.concatStringsSep " " ([
    # first run of mksquashfs copies the nix/store closure and additional files
    "$(sort -u closure)"
    "$out"

    # additional files
    (lib.concatMapStrings (x: " -p ${lib.escapeShellArg x}") [
      # symlink entrypoint to the executable to run
      "entrypoint s 555 0 0 ${program}"
    ])
    "-pf pseudo"
    "-action-file actions"

    "-no-strip" # don't strip leading dirs, to preserve the fact that everything's in the nix store
  ] ++ commonArgs)}

  mksquashfs ${builtins.concatStringsSep " " ([
    # second run of mksquashfs adds the apprun
    # no -no-strip since we *do* want to strip leading dirs now
    "${mkappimage-apprun}/*"
    "$(find extras -mindepth 1 -maxdepth 1)" # to include .DirIcon
    "$out"
    "-no-recovery" # i don't know what a recovery file is but it gives "No such file or directory"
  ] ++ commonArgs)}

  # add the runtime to the start
  dd if=${lib.escapeShellArg mkappimage-runtime} of=$out conv=notrunc

  # make executable
  chmod 755 $out
''
