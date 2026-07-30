#!/bin/bash
# Package Quake HD (org.webosinternals.sdlquakehd) -> installable IPK.
#
# TouchPad only, rendering at the panel's native 1024x768. Its own app id so it
# installs alongside the long-stable phone release rather than replacing it;
# metadata.json gates it to device 101.
#
# Same payload shape as the standard package (binary + game data + touch overlay
# art + the maintainer scripts that open /dev/input to the jail for controllers).
set -e

APPID="org.webosinternals.sdlquakehd"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$HERE/../.."
SRCDIR="$HERE/hd"
BINARY="$HERE/fbuild/webos-gl/quakehd.bin"
STAGING="$HERE/hd-staging"

[ -f "$BINARY" ] || { echo "ERROR: $BINARY not found -- run build-webos.sh first"; exit 1; }

VERSION=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$SRCDIR/appinfo.json")
OUTFILE="${APPID}_${VERSION}_armv7.ipk"
APPDIR="$STAGING/usr/palm/applications/$APPID"

echo "=== Quake HD packaging (v$VERSION, 1024x768) ==="
rm -rf "$STAGING"
mkdir -p "$APPDIR" "$STAGING/CONTROL"

# Payload: the HD app identity, plus the same runtime files the standard build
# ships. Game data must be included -- the jail only exposes the app's own
# directory, so it cannot read the other package's id1/.
# NOTE: deliberately not shipping metadata.json yet. It gates the app to
# device 101 (TouchPad), but we are still isolating why the compositor hands
# this app a 320x480 phone-sized GL surface where the reference app gets
# 1024x768, and the reference ships no metadata.json.
cp "$SRCDIR/appinfo.json" "$APPDIR/"
for item in icon.png index.html README package.properties sources.json app images id1; do
    cp -r "$REPO/$item" "$APPDIR/"
done

# "main" is the NATIVE BINARY, with no shell wrapper. The installer writes an
# LS2 role file naming <appdir>/quakehd, and the bus matches it against
# /proc/<pid>/exe -- a wrapper script would leave the real process running as
# quakehd.bin, which has no role, so PDL fails to register on the bus. The
# binary chdirs to its own directory itself (see sys_sdl.c).
cp -p "$BINARY" "$APPDIR/quakehd"
/opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-strip "$APPDIR/quakehd" 2>/dev/null || true
chmod 755 "$APPDIR/quakehd"

# Strip anything that must never reach a device (see package-webos.sh).
find "$APPDIR" \( -type d \( -name '_meta' -o -name '.git' -o -name '.svn' \) \) \
    -prune -exec rm -rf {} + 2>/dev/null || true
find "$APPDIR" -type f \( -name '.gitignore' -o -name '.DS_Store' -o -name '*~' \) \
    -delete 2>/dev/null || true

cat > "$STAGING/CONTROL/control" <<EOF
Package: $APPID
Version: $VERSION
Architecture: armv7
Maintainer: WebOS Internals <http://www.webos-internals.org>
Description: Quake HD
Section: Games
Priority: optional
Depends:
Source: { "Source":"git://git.webos-internals.org/games/sdlquake.git", "Feed":"WebOS Internals", "Type":"Application", "Category":"Games", "Title":"Quake HD", "FullDescription":"Quake for the HP TouchPad, rendering at the panel's native 1024x768 instead of the old 480x320 phone resolution. USB and Bluetooth game controllers and keyboards supported. TouchPad only.", "Homepage":"http://www.webos-internals.org/wiki/Application:Quake", "License":"id Software License" }
EOF

# Maintainer scripts, retargeted from the standard package to this app id.
for s in postinst prerm; do
    sed "s/org\.webosinternals\.sdlquake\b/$APPID/g; s/99-sdlquake-pad/99-sdlquakehd-pad/g; s/\.sdlquake-orig/.sdlquakehd-orig/g" \
        "$HERE/control/$s" > "$STAGING/CONTROL/$s"
    chmod 755 "$STAGING/CONTROL/$s"
    echo "Including maintainer script: $s"
done

echo "Creating archives..."
cd "$STAGING"
tar czf data.tar.gz ./usr
tar czf control.tar.gz -C CONTROL .
echo "2.0" > debian-binary
ar -cr "../$OUTFILE" debian-binary control.tar.gz data.tar.gz
cd ..
rm -rf "$STAGING"

echo ""
echo "Package ready: $OUTFILE  ($(du -h "$OUTFILE" | cut -f1))"
