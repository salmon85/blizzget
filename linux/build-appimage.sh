#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-${XDG_CACHE_HOME:-$HOME/.cache}}/blizzget-native-appimage-build"
APPDIR="$WORK/BlizzGet.AppDir"
OUT="${1:-$ROOT/dist/BlizzGet-Linux-x86_64.AppImage}"
APPIMAGETOOL_SHA256="e8c1169c4af66c3184d7ee110713d4be04befe671fa358e2c1591ee840553b03"
RUNTIME_SHA256="617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46"

rm -rf "$WORK"
mkdir -p "$WORK" "$APPDIR/usr" "$(dirname "$OUT")"

printf 'Building native C++ executable...\n'
cmake -S "$ROOT/native" -B "$ROOT/build-native" -DCMAKE_BUILD_TYPE=Release \
  -DSYSTEM_CURL_LIBRARY=/usr/lib/x86_64-linux-gnu/libcurl.so.4
cmake --build "$ROOT/build-native" -j"$(nproc)"
if [ -x /usr/bin/ctest ]; then
  /usr/bin/ctest --test-dir "$ROOT/build-native" --output-on-failure
else
  ctest --test-dir "$ROOT/build-native" --output-on-failure
fi
cmake --install "$ROOT/build-native" --prefix "$APPDIR/usr"

install -Dm755 "$ROOT/linux/AppRun" "$APPDIR/AppRun"
install -Dm644 "$ROOT/linux/blizzget.desktop" "$APPDIR/blizzget.desktop"
install -Dm644 "$ROOT/linux/blizzget.desktop" "$APPDIR/usr/share/applications/blizzget.desktop"
install -Dm644 "$ROOT/linux/blizzget.png" "$APPDIR/blizzget.png"
install -Dm644 "$ROOT/linux/blizzget.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/blizzget.png"

printf 'Bundling GTK and native libraries...\n'
mkdir -p "$APPDIR/usr/lib"
ldd "$APPDIR/usr/bin/blizzget" | awk '/=> \// {print $3}' | while read -r lib; do
  case "$lib" in
    */libc.so.6|*/libm.so.6|*/ld-linux*|*/libpthread.so*|*/libdl.so*|*/librt.so*) continue ;;
    /usr/local/*) continue ;;
  esac
  cp -L "$lib" "$APPDIR/usr/lib/"
done
cp -L /usr/lib/x86_64-linux-gnu/libcurl.so.4 "$APPDIR/usr/lib/"
cp -L /usr/lib/x86_64-linux-gnu/libz.so.1 "$APPDIR/usr/lib/"

for dir in \
  /usr/lib/x86_64-linux-gnu/gtk-3.0 \
  /usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0 \
  /usr/share/themes/Adwaita \
  /usr/share/icons/Adwaita \
  /usr/share/glib-2.0/schemas \
  /usr/share/fonts/truetype/dejavu; do
  if [ -e "$dir" ]; then
    case "$dir" in
      /usr/lib/x86_64-linux-gnu/*)
        mkdir -p "$APPDIR/usr/lib/$(dirname "${dir#/usr/lib/x86_64-linux-gnu/}")"
        cp -a "$dir" "$APPDIR/usr/lib/${dir#/usr/lib/x86_64-linux-gnu/}"
        ;;
      *)
        mkdir -p "$APPDIR/usr/$(dirname "${dir#/usr/}")"
        cp -a "$dir" "$APPDIR/usr/${dir#/usr/}"
        ;;
    esac
  fi
done

mkdir -p "$APPDIR/usr/share/fontconfig"
cat > "$APPDIR/usr/share/fontconfig/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <dir prefix="relative">../fonts/truetype/dejavu</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
</fontconfig>
EOF

LOADERS="$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders"
if [ -d "$LOADERS" ] && command -v gdk-pixbuf-query-loaders >/dev/null 2>&1; then
  gdk-pixbuf-query-loaders "$LOADERS"/*.so > "$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
fi

printf 'Checking bundled native GUI under Xvfb...\n'
set +e
GDK_BACKEND=x11 xvfb-run -a timeout 5s "$APPDIR/AppRun" >"$WORK/gui-smoke.log" 2>&1
gui_status=$?
set -e
if [ "$gui_status" -ne 124 ]; then
  cat "$WORK/gui-smoke.log"
  exit 1
fi

printf 'Downloading appimagetool...\n'
if command -v gh >/dev/null 2>&1; then
  gh release download continuous --repo AppImage/appimagetool \
    --pattern 'appimagetool-x86_64.AppImage' --dir "$WORK" --clobber
else
  curl --fail --location --retry 3 \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
    --output "$WORK/appimagetool-x86_64.AppImage"
fi
chmod +x "$WORK/appimagetool-x86_64.AppImage"
printf '%s  %s\n' "$APPIMAGETOOL_SHA256" "$WORK/appimagetool-x86_64.AppImage" | sha256sum -c -

printf 'Downloading AppImage runtime...\n'
if command -v gh >/dev/null 2>&1; then
  gh release download continuous --repo AppImage/type2-runtime \
    --pattern 'runtime-x86_64' --dir "$WORK" --clobber
else
  curl --fail --location --retry 3 \
    https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 \
    --output "$WORK/runtime-x86_64"
fi
printf '%s  %s\n' "$RUNTIME_SHA256" "$WORK/runtime-x86_64" | sha256sum -c -

printf 'Building %s...\n' "$OUT"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$WORK/appimagetool-x86_64.AppImage" \
  --runtime-file "$WORK/runtime-x86_64" "$APPDIR" "$OUT"
printf 'Built: %s\n' "$OUT"
