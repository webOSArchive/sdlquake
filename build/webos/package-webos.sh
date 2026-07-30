#!/bin/bash
# Package SDL Quake for the HP webOS TouchPad -> an installable IPK.
# Payload = the repo-root app files + the freshly built ARM binary; control =
# the maintainer scripts that enable controllers/keyboards in the PDK jail.

set -e

APPID="org.webosinternals.sdlquake"
REPO="../.."                       # repo root (holds appinfo.json, id1/, etc.)
BINARY="fbuild/webos/sdlquake.bin"
STAGING="webos-staging"

echo "=== SDL Quake webOS packaging ==="

if [ ! -f "$BINARY" ]; then
    echo "ERROR: $BINARY not found -- run build-webos.sh first"; exit 1
fi

VERSION=$(grep '"version"' "$REPO/appinfo.json" | sed 's/.*"version": *"\([^"]*\)".*/\1/')
OUTFILE="${APPID}_${VERSION}_armv7.ipk"
APPDIR="$STAGING/usr/palm/applications/$APPID"

rm -rf "$STAGING"
mkdir -p "$APPDIR" "$STAGING/CONTROL"

# --- Payload: copy the app files from the repo root ---
# This list is an ALLOW-list, so anything repo-only is excluded just by not being
# named: _meta (store artwork + screenshots the game never loads), .git, src,
# build, ipks and the *.md docs. Only the four touch-overlay PNGs in images/ are
# actually loaded at runtime -- see JOY_IMAGE_FILENAME etc. in src/vid_sdl.c.
echo "Staging app payload..."
for item in appinfo.json sdlquake sdlquake.bin icon.png index.html README \
            package.properties sources.json app images id1; do
    cp -r "$REPO/$item" "$APPDIR/"
done

# Safety net for what the allow-list alone cannot catch: these names are stripped
# from ANYWHERE in the staged tree, so a nested copy (images/_meta, app/.git) or
# a future edit that widens the list above still cannot quietly ship them. The
# patterns are quoted so the shell cannot glob them against the build directory.
PRUNED=$(cd "$APPDIR" && find . \
    \( -type d \( -name '_meta' -o -name '.git' -o -name '.svn' \) \) -o \
    \( -type f \( -name '.gitignore' -o -name '.gitattributes' \
                  -o -name '.DS_Store' -o -name 'Thumbs.db' \
                  -o -name '*~' -o -name '*.orig' -o -name '*.rej' \) \) \
    2>/dev/null | sed 's|^\./||' | sort)
if [ -n "$PRUNED" ]; then
    echo "Excluding from payload:"
    echo "$PRUNED" | sed 's/^/    /'
    find "$APPDIR" \( -type d \( -name '_meta' -o -name '.git' -o -name '.svn' \) \) \
        -prune -exec rm -rf {} + 2>/dev/null || true
    find "$APPDIR" -type f \( -name '.gitignore' -o -name '.gitattributes' \
        -o -name '.DS_Store' -o -name 'Thumbs.db' -o -name '*~' \
        -o -name '*.orig' -o -name '*.rej' \) -delete 2>/dev/null || true
fi

# Overwrite the checked-in binary with the freshly built one.
cp -p "$BINARY" "$APPDIR/sdlquake.bin"
/opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-strip "$APPDIR/sdlquake.bin" 2>/dev/null || true
chmod 755 "$APPDIR/sdlquake" "$APPDIR/sdlquake.bin"

# --- Control ---
cat > "$STAGING/CONTROL/control" <<EOF
Package: $APPID
Version: $VERSION
Architecture: armv7
Maintainer: WebOS Internals <http://www.webos-internals.org>
Description: Quake
Section: Games
Priority: optional
Depends:
Source: { "Source":"git://git.webos-internals.org/games/sdlquake.git", "Feed":"WebOS Internals", "Type":"Application", "Category":"Games", "Title":"Quake", "FullDescription":"Port of the popular Quake 3-D shooter by id Software.<br>1.5.0: rebuilt the controller mapping around per-pad profiles + one shared scheme, so every pad behaves the same; adds the ShanWan/Xbox knock-off (analog triggers on ABS_GAS/ABS_BRAKE) and the DragonRise/Saturn pad, plus a generic fallback; ignores phantom axes that pegged movement on; unknown pads now auto-detect sticks, d-pads and analog triggers and can be remapped in-game (padstatus/padtest/padbtn/padaxis, see CONTROLLERS.md) with no rebuild; in-game actions no longer fire while a menu is open.<br>1.4.3: fix launcher crash (UDP_Init NULL-deref when jailed) + unsigned char + lighter controller scan.<br>1.4.2: fix glibc mismatch (build with PDK toolchain, GLIBC_2.4).<br>1.4.1: postinst hardened (no blocking sync/udev reload).<br>1.4.0: USB & Bluetooth game controller + keyboard support (direct evdev); the on-screen overlay now works the menus (fire selects, top-edge is Escape); forced fullscreen; PDL orientation fix.<br>1.3.0: Converted to PDK framework.", "Homepage":"http://www.webos-internals.org/wiki/Application:Quake", "License":"id Software License" }
EOF

for s in postinst prerm; do
    cp "control/$s" "$STAGING/CONTROL/$s"
    chmod 755 "$STAGING/CONTROL/$s"
    echo "Including maintainer script: $s"
done

echo "Creating archives..."
cd "$STAGING"
tar czf data.tar.gz ./usr
tar czf control.tar.gz -C CONTROL .
echo "2.0" > debian-binary

echo "Assembling IPK..."
ar -cr "../$OUTFILE" debian-binary control.tar.gz data.tar.gz
cd ..
rm -rf "$STAGING"

echo ""
echo "Package ready: build/webos/$OUTFILE  ($(du -h "$OUTFILE" | cut -f1))"
