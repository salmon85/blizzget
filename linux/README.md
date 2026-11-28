# BlizzGet Linux

This branch replaces the Python wrapper with a native C++/GTK3 application.
The downloader, NGDP/CASC parsing, CDN range requests, local storage writer,
version-history client, and GUI are all C++.

The AppImage downloads files from Blizzard's official CDN. It does not contain
game assets and requires ownership of the game.

## Highlights

- Produces a complete local CASC `Data` store, not just extracted game files.
  The client can open the result directly through Proton.
- Writes guarded V7 local indexes with the reserved 48 KiB update/KMT area and
  64 KiB alignment required by the retail CASC reader.
- Generates the correct `.build.info` schema for the selected client: legacy
  14-column for Diablo III 2.7.4 and modern 15-column for 2.8.0.
- Downloads only the selected platform, architecture, locale, and install-tag
  content by default; the full CDN remains an explicit option.
- Resumes interrupted loose-file downloads with HTTP ranges and verifies
  restarted installs by size and MD5.
- Uses parallel CDN, archive, CASC, and final-extraction workers while keeping
  shared index/data writes serialized.
- Supports a searchable BlizzTrack catalog, historical versions, manual build
  hashes, encrypted-product visibility, themes, connection limits, and speed
  caps.

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

CASC storage is written while archive chunks are still downloading. Archive
indices are fetched with up to 16 parallel connections. CASC processing uses
separate pools: up to eight loose-file workers and four archive-backed workers,
so network downloads and local extraction overlap. The shared data stream and
index updates remain serialized for archive integrity. Loose-only files are
streamed directly into an atomic cache file, resumed with HTTP ranges after a
failure, and retried up to eight times with capped exponential backoff. Each
worker reuses its HTTP connection between requests. The CASC progress line
shows processed bytes, a rolling transfer rate, and a recent-throughput ETA. On
a restarted install, existing extracted files are checked by size and MD5
against the install manifest; matching files are skipped, while missing or
changed files are extracted again. Final extraction also uses up to eight
workers, with each file verified independently before being written.

**Download optional high-resolution assets** is enabled by default. Disabling
it keeps required priority 0–1 content and skips priority-2 optional assets,
saving roughly 4.6 GB for Diablo III.

**Download entire CDN for this game** is off by default. Enabling it ignores
platform, locale, and priority filters and downloads every content entry for
the selected build.

## Install compatibility

BlizzGet writes the local CASC store directly while downloading. The final
`Data/data/*.idx` files use the guarded V7 local format, reserve the 48 KiB
update/KMT region the retail reader needs, and are padded to a 64 KiB boundary.
This is the difference between a readable data directory and a store the client
reports as `NeedsRepair` or rejects with `_Bind data error 6`.

The generated `.build.info` is matched to the client executable rather than
guessed from the product name. BlizzGet selects the main client `.exe` from the
install manifest, decodes it, and scans for the legacy `KeyService` token or the
modern `CDN Servers` token. It then writes:

- legacy 14-column `.build.info` for 2.7.4 and earlier;
- modern 15-column `.build.info` for 2.8.0 and later.

If neither token is conclusive, BlizzGet falls back to its version rule and
logs the fallback. A fresh extraction can therefore produce a launchable
install without copying files from an existing Battle.net installation.

Verified under GE-Proton 11-13:

- Diablo III `2.7.4.84161` — reaches `BattleNet_MainScreen`, private-server login.
- Diablo III `2.8.0.99920` — reaches `BattleNet_MainScreen`, 209,014 assets.

The test suite covers the index layout and hashes, `.build.info` schema
detection, catalog merging/search, cache clearing, concurrent storage writes,
and extraction retries. Run it with:

```bash
/usr/bin/ctest --test-dir build-native --output-on-failure
```

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
