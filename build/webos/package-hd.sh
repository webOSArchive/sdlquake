#!/bin/bash
# Package Quake HD (org.webosarchive.sdlquakehd) -> installable IPK.
#
# TouchPad only, rendering at the panel's native 1024x768. Its own app id so it
# installs alongside the long-stable phone release rather than replacing it.
# It deliberately ships NO metadata.json -- see the long note below, that file
# is what would force the app into 320x480 phone-compatibility mode.
#
# Same payload shape as the standard package (binary + game data + touch overlay
# art + the maintainer scripts that open /dev/input to the jail for controllers).
set -e

APPID="org.webosarchive.sdlquakehd"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$HERE/../.."
SRCDIR="$HERE/hd"
BINARY="$HERE/fbuild/webos-gl/quakehd.bin"
STAGING="$HERE/hd-staging"

[ -f "$BINARY" ] || { echo "ERROR: $BINARY not found -- run ./build-webos-gl.sh first"; exit 1; }

VERSION=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$SRCDIR/appinfo.json")
OUTFILE="${APPID}_${VERSION}_armv7.ipk"
APPDIR="$STAGING/usr/palm/applications/$APPID"

echo "=== Quake HD packaging (v$VERSION, 1024x768) ==="
rm -rf "$STAGING"
mkdir -p "$APPDIR" "$STAGING/CONTROL"

# Payload: the HD app identity, plus the same runtime files the standard build
# ships. Game data must be included -- the jail only exposes the app's own
# directory, so it cannot read the other package's id1/.
# DO NOT ship metadata.json. Gating with {"version":1,"devices":[101]} -- 101
# being the TouchPad -- puts the app into PHONE COMPATIBILITY mode: the
# compositor then hands it a 320x480 surface instead of the panel's native
# 1024x768. Verified by A/B on the same binary, dropping the file in and out:
#   with metadata.json    -> GL surface 320x480
#   without metadata.json -> GL surface 1024x768
# The known-good GLES reference on this device (Tux Racer) ships none either.
# The app is TouchPad-only by nature (it needs the GPU and the resolution);
# gating, if wanted, needs some other mechanism.
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
Maintainer: webOS Archive <https://webosarchive.org>
Description: Quake HD
Section: Games
Priority: optional
Depends:
Source: { "Source":"https://github.com/webOSArchive/webos-sdlquake-hd.git", "Feed":"webOS Archive", "Type":"Application", "Category":"Games", "Title":"Quake HD", "FullDescription":"Quake for the HP TouchPad, rendering at the panel's native 1024x768 instead of the old 480x320 phone resolution. USB and Bluetooth game controllers and keyboards supported. TouchPad only.", "Homepage":"https://webosarchive.org", "License":"id Software License" }
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
# Write straight into the repo's ipks/ -- the ONE place packages live. An
# earlier version also left a copy next to this script, and two files with the
# same name in two directories is precisely how a stale build gets installed
# without anyone noticing. There is now nothing to keep in sync.
mkdir -p "$REPO/ipks"
ar -cr "$REPO/ipks/$OUTFILE" debian-binary control.tar.gz data.tar.gz
cd ..
rm -rf "$STAGING"

echo ""
echo "Package ready: $REPO/ipks/$OUTFILE  ($(du -h "$REPO/ipks/$OUTFILE" | cut -f1))"
echo "Install with:  palm-install ipks/$OUTFILE"
