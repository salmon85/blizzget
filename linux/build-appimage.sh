#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${TMPDIR:-${XDG_CACHE_HOME:-$HOME/.cache}}/blizzget-appimage-build"
APPDIR="$WORK/BlizzGet.AppDir"
OUT="${1:-$ROOT/dist/BlizzGet-Linux-x86_64.AppImage}"
CASCETTE_COMMIT="dc0dfb89b146d80b363741de25c2be340c2da8fd"
PYTHON_VERSION="3.12"

rm -rf "$WORK"
mkdir -p "$WORK" "$(dirname "$OUT")"

printf 'Cloning Cascette at %s...\n' "$CASCETTE_COMMIT"
git clone --quiet https://github.com/wowemulation-dev/cascette-py.git "$WORK/cascette-py"
git -C "$WORK/cascette-py" checkout --quiet "$CASCETTE_COMMIT"
git -C "$WORK/cascette-py" apply "$ROOT/cascette-d3.patch"

printf 'Installing portable Python %s...\n' "$PYTHON_VERSION"
uv python install "$PYTHON_VERSION" --install-dir "$WORK/python"
PYROOT="$(find "$WORK/python" -mindepth 1 -maxdepth 1 -type d -name 'cpython-*' | head -n1)"
test -n "$PYROOT"
test -x "$PYROOT/bin/python3.12"

mkdir -p "$APPDIR/usr"
cp -a "$PYROOT/." "$APPDIR/usr/"
mkdir -p "$APPDIR/usr/lib/python3.12/site-packages"

printf 'Installing Cascette and dependencies into the AppDir...\n'
uv pip install --python "$APPDIR/usr/bin/python3.12" \
  --target "$APPDIR/usr/lib/python3.12/site-packages" \
  "$WORK/cascette-py"

install -Dm755 "$ROOT/linux/AppRun" "$APPDIR/AppRun"
install -Dm644 "$ROOT/linux/blizzget.desktop" "$APPDIR/blizzget.desktop"
install -Dm644 "$ROOT/linux/blizzget.desktop" \
  "$APPDIR/usr/share/applications/blizzget.desktop"
install -Dm644 "$ROOT/linux/blizzget.png" "$APPDIR/blizzget.png"
install -Dm644 "$ROOT/linux/blizzget.png" \
  "$APPDIR/usr/share/icons/hicolor/256x256/apps/blizzget.png"
install -Dm755 "$ROOT/linux/blizzget_gui.py" \
  "$APPDIR/usr/share/blizzget/blizzget_gui.py"
install -Dm644 "$ROOT/linux/blizzget.png" \
  "$APPDIR/usr/share/blizzget/blizzget.png"
install -Dm644 "$WORK/cascette-py/LICENSE-MIT" \
  "$APPDIR/usr/share/doc/blizzget/Cascette-LICENSE-MIT"
install -Dm644 "$WORK/cascette-py/LICENSE-APACHE" \
  "$APPDIR/usr/share/doc/blizzget/Cascette-LICENSE-APACHE"

printf 'Checking the bundled CLI...\n'
APPIMAGE_EXTRACT_AND_RUN=1 "$APPDIR/AppRun" --help >/dev/null
APPIMAGE_EXTRACT_AND_RUN=1 "$APPDIR/AppRun" version

if command -v xvfb-run >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
  printf 'Checking the bundled GUI under Xvfb...\n'
  set +e
  timeout 5s xvfb-run -a "$APPDIR/AppRun" --gui >"$WORK/gui-smoke.log" 2>&1
  gui_status=$?
  set -e
  if [ "$gui_status" -ne 124 ]; then
    cat "$WORK/gui-smoke.log"
    exit 1
  fi
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

printf 'Downloading AppImage runtime...\n'
if command -v gh >/dev/null 2>&1; then
  gh release download continuous --repo AppImage/type2-runtime \
    --pattern 'runtime-x86_64' --dir "$WORK" --clobber
else
  curl --fail --location --retry 3 \
    https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 \
    --output "$WORK/runtime-x86_64"
fi

printf 'Building %s...\n' "$OUT"
ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$WORK/appimagetool-x86_64.AppImage" \
  --runtime-file "$WORK/runtime-x86_64" "$APPDIR" "$OUT"
printf 'Built: %s\n' "$OUT"
