{ fetchFromGitHub
, stdenv
, fuse3
, pkg-config
, squashfuse
, zstd
, zlib
, xz
, lz4
, lzo
}:

let
  rev = "8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa";

  src = fetchFromGitHub {
    owner = "AppImage";
    repo = "type2-runtime";
    inherit rev;
    hash = "sha256-+ffBk9lnMnVz4uq+FAYk0h/uMzkTVwfoPuMbT0twoFQ=";
  };

  fuse3' = fuse3.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [
      # use the fusermount that the runtime finds (which may be fuse 2's
      # fusermount, or in a different directory), via $FUSERMOUNT_PROG
      "${src}/patches/libfuse/mount.c.diff"
    ];
  });

  squashfuse' = (squashfuse.override {
    fuse3 = fuse3';
  }).overrideAttrs (old: {
    postInstall = (old.postInstall or "") + ''
      cp *.h -t $out/include/squashfuse/
    '';
  });
in
stdenv.mkDerivation {
  pname = "appimage-type2-runtime";
  version = "unstable-2026-09-28";

  inherit src;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    fuse3'
    squashfuse'
    zstd
    zlib
    xz
    lz4
    lzo
  ];

  patchPhase = ''
    # fixes for the keepalive pipe, TMPDIR, --appimage-extract and
    # --appimage-extract-and-run
    patch -p1 < ${./fixes.patch}
  '';

  configurePhase = ''
    $PKG_CONFIG --cflags fuse3 > cflags
  '';

  buildPhase = ''
    $CC src/runtime/runtime.c -o $out \
      -D_FILE_OFFSET_BITS=64 -DGIT_COMMIT='"${builtins.substring 0 7 rev}"' \
      $(cat cflags) \
      -std=gnu99 -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -static -Wall -Werror \
      -lsquashfuse -lsquashfuse_ll -lfuse3 -lzstd -lz -llzma -llz4 -llzo2 \
      -T src/runtime/data_sections.ld

    # Add AppImage Type 2 Magic Bytes to runtime
    printf %b '\x41\x49\x02' > magic_bytes
    dd if=magic_bytes of=$out bs=1 count=3 seek=8 conv=notrunc status=none
  '';

  dontFixup = true;
}
