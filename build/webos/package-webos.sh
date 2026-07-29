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
echo "Staging app payload..."
for item in appinfo.json sdlquake sdlquake.bin icon.png index.html README \
            package.properties sources.json app images id1; do
    cp -r "$REPO/$item" "$APPDIR/"
done

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
Source: { "Source":"git://git.webos-internals.org/games/sdlquake.git", "Feed":"WebOS Internals", "Type":"Application", "Category":"Games", "Title":"Quake", "FullDescription":"Port of the popular Quake 3-D shooter by id Software.<br>1.4.3: fix launcher crash (UDP_Init NULL-deref when jailed) + unsigned char + lighter controller scan.<br>1.4.2: fix glibc mismatch (build with PDK toolchain, GLIBC_2.4).<br>1.4.1: postinst hardened (no blocking sync/udev reload).<br>1.4.0: USB & Bluetooth game controller + keyboard support (direct evdev); the on-screen overlay now works the menus (fire selects, top-edge is Escape); forced fullscreen; PDL orientation fix.<br>1.3.0: Converted to PDK framework.", "Homepage":"http://www.webos-internals.org/wiki/Application:Quake", "License":"id Software License" }
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
