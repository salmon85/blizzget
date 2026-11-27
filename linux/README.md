# BlizzGet Linux

This branch replaces the Python wrapper with a native C++/GTK3 application.
The downloader, NGDP/CASC parsing, CDN range requests, local storage writer,
version-history client, and GUI are all C++.

The AppImage downloads files from Blizzard's official CDN. It does not contain
game assets and requires ownership of the game.

## Run

Double-click the AppImage, or run:

```bash
./BlizzGet-Linux-x86_64.AppImage
```

The Product dropdown uses a bundled fallback catalog of verified unencrypted
products and automatically merges the latest catalog from BlizzTrack. The
adjacent search field filters by product name or code as you type; clearing it
restores the previous selection. Encrypted products are hidden by default;
enable **Show encrypted products** to display them. Use **Update Catalog** to
refresh manually. The live catalog and per-product CDN paths are cached for
24 hours; if BlizzTrack is unavailable, the expanded bundled fallback remains
usable.

The Version dropdown loads historical versions indexed by BlizzTrack for the
selected game and region. The list is cached locally and can be refreshed with
the Refresh button. Advanced exposes manual build/CDN hashes.

The dark/light button switches themes. Download connection limits and speed
caps are configurable. The defaults are 48 global connections and 12 per host.

The Cache directory field controls the CDN archive cache, cached version lists,
BlizzTrack catalog, and resolved CDN paths. It defaults to `~/.cache/cascette`.
Enable Clear cache on close to remove BlizzGet's `cdn`, `versions`, `catalog`,
and `cdn-paths` cache contents after the downloader has stopped. BlizzGet asks
for confirmation and preserves unrelated files in the selected directory.

## Build locally

Requirements on Debian/Ubuntu:

```bash
sudo apt-get install -y build-essential cmake pkg-config \
  libgtk-3-dev libjson-glib-dev libcurl4-openssl-dev liblz4-dev \
  zlib1g-dev libfuse2 file xvfb
```

Build the AppImage:

```bash
./linux/build-appimage.sh
```

The output is written to `dist/BlizzGet-Linux-x86_64.AppImage`.

For a local native binary without packaging:

```bash
cmake -S native -B build-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-native
./build-native/blizzget
```

## Source layout

- `native/app.cpp` contains the GTK3 interface and asynchronous UI updates.
- `native/downloader.cpp` contains BlizzTrack history, NGDP manifests, archive
  range downloads, cache handling, and installation.
- `src/ngdp.cpp` contains BLTE, encoding-table, and local CASC storage support.
- `src/base/` contains the portable C++ file, HTTP, path, checksum, and string
  layers ported from upstream BlizzGet.
- `linux/build-appimage.sh` builds the C++ binary, bundles GTK and its native
  libraries, and creates the AppImage.
- `linux/AppRun` launches the bundled native binary with its private library,
  schema, font, and pixbuf paths.

The native AppImage contains no Python runtime or Python GUI wrapper.
