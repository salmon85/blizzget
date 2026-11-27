# blizzget

[Download](https://github.com/salmon85/blizzget/releases)

Blizzard CDN downloader. Capable of downloading upcoming versions of games. **They will most likely not be playable, but can be used for datamining and such** - if you're going to try to play the game, make sure you run it through battle.net app so it can fix the possible inconsistencies. Note that it always stores the CASC archive in the /Data folder, which is not correct for every game - you might want to rename the folder to what the actual game uses. Currently I only tested it for Diablo III.

The program represents a simple 4-step wizard.

Step 1: select game and region.  
Step 2: select build version.  
Step 3: select a combination of download tags - these are used to specify the platform, language, etc.  
Step 4: select download location and start the download.

Note that the program creates a cache in its local directory, which significantly increases disk usage (doubles or even triples, because it often loads partial archives and reserves space for the entire file), but re-downloading the game, or downloading future versions can take significantly less time. You can delete the cache folder after downloading if you value disk space over future network traffic.

Using this is not recommended for playable versions, as it is probably slower than the battle.net launcher (due to only using one download thread), and might not install everything correctly. It is also incapable of patching (aka upgrading a previous installation). You might be able to use it to pre-download an upcoming game and then use the battle.net launcher to 'fix' it (make sure to rename the Data folder to what is appropriate for the specific game, though.. in some cases it even has to be inside another folder, i.e. /data/casc for Overwatch).

## Linux release

This fork adds a native Linux AppImage with a C++/GTK3 GUI and downloader.
See [linux/README.md](linux/README.md) for build and usage instructions.
The upstream Win32 source remains in `src/` and `vc13/`, but the packaged Linux
release contains no Python or Tkinter wrapper.

<hr/>

<b>Overwatch Beta</b>
1. Download a pre built app from the releases tab, or compile with VS2022.
2. Open the upstream BlizzGet app and select program code: `prob - Overwatch Beta` and your region.
3. Select the available build ID; `build-branch` should look like `2_00_8_0`.
4. Wait for the encoding table and download list.
5. Select your language and destination folder, then click Start.
6. Wait for the download to complete. Login still requires an account allowed to join the relevant service.
