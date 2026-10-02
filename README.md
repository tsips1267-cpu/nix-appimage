# nix-appimage

Create an AppImage, bundling a derivation and all its dependencies into a single-file executable.
Like [nix-bundle](https://github.com/matthewbauer/nix-bundle), but much faster and without the glibc dependency.

## Getting started

To use this, you will need to have [Nix](https://nixos.org/) available.
Then, run this via the [nix bundle](https://nixos.org/manual/nix/unstable/command-ref/new-cli/nix3-bundle.html) interface, replacing `nixpkgs#hello` with the flake you want to build:

```
$ nix bundle --bundler github:ralismark/nix-appimage nixpkgs#hello
```

This produces `hello.AppImage`, which prints "Hello, world!" when run:

```
$ ./hello.AppImage
Hello, world!
```

If you get an `entrypoint ... is not an executable file` error, or want to specify a different binary to run, you can instead use the `./bundle` script:

```
$ ./bundle dnsutils /bin/dig # or ./bundle dnsutils dig
$ ./dig.AppImage -v
DiG 9.18.14
```

Note that the package (`dnsutils` here) is a package in `<nixpkgs>` (i.e. the nixpkgs in your `NIX_PATH`), rather than a flake reference like `nixpkgs#dnsutils`.

You can also use nix-appimage as a nix library -- the flake provides `lib.<system>.mkAppImage` which supports more options.
See mkAppImage.nix for details.

## Graphics (OpenGL, EGL, Vulkan)

Programs that use the GPU work out of the box on other distros, without needing something like [nixGL](https://github.com/guibou/nixGL).

Normally this is a [known problem](https://github.com/NixOS/nixpkgs/issues/9415): nix-built programs look for GPU drivers in `/run/opengl-driver`, which only exists on NixOS.
To fix this, nix-appimage bundles [Mesa](https://mesa3d.org/) (which supports Intel, AMD, Nouveau, virtual GPUs and software rendering) and, on x86_64, Intel's VA-API video decoding drivers (Mesa's only cover AMD and Nouveau), and AppRun provides them to the bundled program at `/run/opengl-driver`.
NVIDIA's proprietary driver can't be bundled, since it has to match the host's kernel module, so if the host has it installed, AppRun makes the host's NVIDIA libraries available to the bundled program as well.
The bundled drivers are used even if the host has its own `/run/opengl-driver` (i.e. on NixOS), since that points into the host's `/nix/store`, which the AppImage replaces with its own.
This also means that on NixOS, the host's NVIDIA driver can't be used.

By default, drivers are only bundled if the program uses OpenGL, EGL, Vulkan, GBM, VA-API or VDPAU (i.e. its closure contains libGL, libEGL, libvulkan, libgbm, libva, etc), since they add roughly 150-180MB to the AppImage.
Note that determining this still requires downloading the drivers when building.
`mkAppImage` has options to control this:

- `graphics`: `"auto"` (the default) to detect whether the program needs drivers, or `true`/`false` to always/never bundle them.
- `graphicsDrivers`: the packages that make up `/run/opengl-driver`, like `hardware.graphics.package` and `hardware.graphics.extraPackages` on NixOS. Defaults to `[ mesa ]` from nix-appimage's nixpkgs, plus `intel-media-driver` and `intel-vaapi-driver` on x86_64.

Some things to be aware of:

- Drivers get loaded into the bundled program, so they should come from a nixpkgs that is no newer than the program's, otherwise they may need a newer glibc than the program has.
  When using `mkAppImage` directly, you can pass `graphicsDrivers = [ pkgs.mesa ]` with the same `pkgs` as the program to avoid this.
  On hosts with NVIDIA's driver, a few libraries from nix-appimage's nixpkgs also get loaded (see graphics/host-driver-deps.nix), which `mkAppImage.override { mkappimage-host-driver-deps = ...; }` can replace.
- Programs that only use CUDA (rather than e.g. OpenGL) aren't detected, so need `graphics = true` to get access to the host's NVIDIA driver.
- GPUs that are newer than the bundled Mesa may not be supported by it, in which case programs fall back to software rendering.
- Hardware video decoding works through VA-API on Intel and AMD GPUs, and through NVDEC (CUDA), VDPAU or Vulkan with NVIDIA's driver.
  Vulkan video decoding in the bundled Mesa (25.0) is only on by default for AMD GPUs from RDNA1 to RDNA3, and can be turned on for Intel GPUs with `ANV_VIDEO_DECODE=1`.
  Programs like mpv try other methods when one doesn't work, so they may print errors about those before finding one that does.
- To add `/run/opengl-driver`, AppRun gives the program its own read-only `/run` containing the host's `/run` entries as of startup (the directories in it are still writable as usual).
  Entries created directly in the host's `/run` after that (or sockets like `/run/docker.sock` that get recreated when their daemon restarts) aren't visible to the program until it's restarted, and the program can't create entries directly in `/run`.
- Vulkan programs also see the host's Vulkan driver manifests (in `/usr/share/vulkan/icd.d`).
  Depending on the distro, this can make each GPU show up twice (both using the bundled driver), or make the Vulkan loader print errors about host drivers it can't load.

## Caveats

We only make a best-effort attempt to copy in the relevant .desktop files and icons, so they may not be present.
This doesn't affect the running of bundled apps, but might cause issues with showing up correctly in application launchers (e.g. rofi).

Since the bundled app sees the AppImage's `/nix/store` instead of the host's, running an AppImage that is itself in the host's `/nix/store` (e.g. `./result`) means `$APPIMAGE` points to a file the app can't see, which matters for apps that relaunch themselves through it.

The current implementation also requires unprivileged Linux User Namespaces, which are available since Linux 3.8 (released in 2013), but may not be enabled for security reasons.
In particular, Ubuntu 23.10 and later restrict what they can do using AppArmor (`kernel.apparmor_restrict_unprivileged_userns`), so AppImages made by nix-appimage only work for normal users there if that's turned off, or allowed for the AppImage by an AppArmor profile.
Running the bundled app this way (see AppRun below) has some side effects:

- Each top-level directory (e.g. `/home` and `/tmp`) is a separate mount for the app, so renaming or hard-linking files between them fails with "Invalid cross-device link" (`EXDEV`), even if they're on the same filesystem.
  Many programs fall back to copying when this happens, but not all do.
- When run by a normal user, only that user's uid and gid are mapped into the user namespace.
  So files owned by other users (including root) appear to be owned by `nobody`, supplementary groups appear as `nogroup`, and setuid programs such as `sudo` don't work.
- Mounts made by the app aren't visible outside it, and `/` itself is read-only.
- The app gets the path of the bundled executable (e.g. `/nix/store/...-hello-2.12.1/bin/hello`) as `argv[0]`, rather than the AppImage's path, which is available as `$APPIMAGE` (and the original `argv[0]` as `$ARGV0`).

The AppImage normally gets mounted using FUSE, which for normal users needs a setuid `fusermount3` or `fusermount` (from either FUSE 3 or FUSE 2) on the `PATH`, which most distros have.
FUSE only lets the user that ran the AppImage access the bundled files.
So when run as root, apps that switch to another user can't access the bundled `/nix/store` anymore, unless the AppImage is run with `--appimage-extract-and-run` instead.
That option (or setting `APPIMAGE_EXTRACT_AND_RUN=1`) is also how to run AppImages where FUSE isn't available (e.g. in containers).
It extracts the AppImage to `$TMPDIR/appimage_extracted_<hash>_<uid>` (or `/tmp/...` if `TMPDIR` isn't set), which later runs reuse, and which isn't deleted afterwards.

## Under The Hood

nix-appimage creates [type 2 AppImages](https://github.com/AppImage/AppImageSpec/blob/ce1910e6443357e3406a40d458f78ba3f34293b8/draft.md#type-2-image-format), which are essentially just a binary, known as the Runtime, concatenated with a squashfs file system.
When the AppImage is run, the runtime simply mounts the squashfs somewhere and runs the contained `AppRun`.
The squashfs contains all the files needed to run the program:

- `nix/store/...`, containing the closure of the bundled program
- `entrypoint`, a symlink to the actual executable, e.g. `/nix/store/q9cqc10sw293xpx3hca4qpsmbg7hsgzy-hello-2.12.1/bin/hello`
- `AppRun`, which gets started after the squashfs is mounted.
  This isn't the actual bundled executable, but a wrapper that makes the bundled nix/store file visible under /nix/store before executing `entrypoint`.
- `graphics/`, if graphics drivers are bundled.
  This contains `drivers`, a symlink to the drivers that AppRun puts at `/run/opengl-driver`, and the information AppRun needs to make the host's NVIDIA driver available.
  For the latter, AppRun finds the host's NVIDIA libraries using the host's ld.so cache, and adds them to the ld.so cache of the bundled glibc (which nixpkgs' glibc reads from `<glibc>/etc/ld.so.cache` rather than `/etc/ld.so.cache`).
  Unlike `LD_LIBRARY_PATH`, this only affects nix-built programs, and doesn't take priority over libraries found through RUNPATH.

Runtimes are included within the flake as `legacyPackages.<system>.appimage-runtimes.<name>`.
Currently supported are:

- `appimage-type2-runtime` (default)
  This is [AppImage/type2-runtime](https://github.com/AppImage/type2-runtime), a static runtime maintained by the official AppImage team.
- `appimagecrafters`.
  This is [AppImageCrafers/appimage-runtime](https://github.com/AppImageCrafters/appimage-runtime), a similar static runtime that was the old default for nix-appimage.

AppRuns are included within the flake as `legacyPackages.<system>.appimage-appruns.<name>`.
Currently supported are:

- `userns-chroot` (default).
  This uses Linux User Namespaces and pivot_root to make /nix/store appear to have the bundled files, similar to [nix-user-chroot](https://github.com/nix-community/nix-user-chroot).
