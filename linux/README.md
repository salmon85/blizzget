# BlizzGet Linux

This branch adds a native Linux command-line build of the Blizzard CDN
downloader functionality. Upstream BlizzGet 1.1 is a Win32 GUI application and
has no Linux target, so the Linux build uses the MIT/Apache-2.0 Cascette
NGDP/CASC installer with a small Diablo III compatibility patch.

The AppImage downloads files from Blizzard's official CDN. It does not contain
game assets and requires ownership of the game.

## Run

Double-click the AppImage to open the GUI, or run it with arguments for CLI use:

```bash
./BlizzGet-Linux-x86_64.AppImage --help
./BlizzGet-Linux-x86_64.AppImage version
./BlizzGet-Linux-x86_64.AppImage --gui
```

Example: download the official Diablo III 2.7.4.84161 client:

```bash
./BlizzGet-Linux-x86_64.AppImage install install-product \
  5194054cfa6480884c9a451b6f1107a3 \
  6547fc64ca796c7c1b25864145814ace \
  "$HOME/Games/Diablo III 2.7.4" \
  --product d3 --region us --platform Windows --arch x86_64 \
  --locale enUS --subfolder ''
```

The old build config is paired with the current CDN config, matching BlizzGet's
behavior. The historical CDN config is no longer served.

## Build locally

Requirements: `git`, `curl`, `uv`, and an x86_64 Linux host.

```bash
./linux/build-appimage.sh
```

The output is written to `dist/BlizzGet-Linux-x86_64.AppImage`.

## Source layout

- `cascette-d3.patch` adds Diablo III product support, correct `.build.info`
  tags/version parsing, tests, and configurable download concurrency.
- `linux/build-appimage.sh` pins Cascette commit
  `dc0dfb89b146d80b363741de25c2be340c2da8fd`, applies the patch, installs a
  portable Python runtime and dependencies, and builds the AppImage.
- `linux/AppRun` launches the bundled CLI without a system Python installation.

Cascette is dual-licensed under MIT or Apache-2.0. Its license texts are
included inside the AppImage and downloaded by the build script.
