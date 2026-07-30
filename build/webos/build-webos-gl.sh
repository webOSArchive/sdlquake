#!/bin/bash
# Build GL Quake for the HP webOS TouchPad -- OpenGL ES 1.1 on the Adreno 220,
# rendering at the panel's native 1024x768.
#
# Same toolchain rules as the software build (see build-webos.sh): the PDK's own
# CodeSourcery gcc, because it targets the device's GLIBC_2.4; do not use Linaro
# (links GLIBC_2.15, binary won't load); no -funroll-loops (ICEs this gcc).
#
# The object list is the GL renderer instead of the software rasterizer:
#   IN  gl_draw gl_mesh gl_model gl_refrag gl_rlight gl_rmain gl_rmisc
#       gl_rsurf gl_screen gl_warp gl_vidsdl gles_compat
#   OUT d_* r_* draw model screen vid_sdl nonintel + the x86 asm stubs
set -e

PDK="/opt/PalmPDK"
TOOLCHAIN_BIN="$PDK/arm-gcc/bin"
CC="$TOOLCHAIN_BIN/arm-none-linux-gnueabi-gcc"

SRC="../../src"
BUILDDIR="fbuild/webos-gl"
OUT="$BUILDDIR/quakehd.bin"

OPTS="-O2 -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp -ffast-math -fsingle-precision-constant"
# Take the version straight from the app descriptor so the update check can
# never compare against a stale hand-copied number.
APPVER=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$(dirname "${BASH_SOURCE[0]}")/hd/appinfo.json")
[ -n "$APPVER" ] || { echo "ERROR: could not read version from hd/appinfo.json"; exit 1; }
echo "App version: $APPVER"

CFLAGS="$OPTS -D__webos__ -DGLQUAKE -DSDL -DELF -D_GNU_SOURCE=1 -D_REENTRANT"
# Unquoted on purpose: CFLAGS is expanded unquoted below, so a quoted
# string could not survive word-splitting. updater.h stringifies it.
CFLAGS="$CFLAGS -DQUAKEHD_VERSION_RAW=$APPVER"
CFLAGS="$CFLAGS -I$PDK/include -I$PDK/include/SDL -I$SRC"

# -lGLES_CM is OpenGL ES 1.1. Do NOT link -lEGL: SDL owns the context, and
# touching EGL directly makes the 3-layer compositor flicker on every touch.
LDFLAGS="-L$PDK/device/lib -Wl,-rpath,/usr/local/lib -lSDL -lSDL_image -lpdl -lGLES_CM -lpthread -lm"

C_SRCS="
  cd_sdl chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console
  crc cvar host host_cmd keys mathlib menu net_bsd net_dgrm net_loop net_main
  net_udp net_vcr net_wso pr_cmds pr_edict pr_exec sbar snd_dma snd_mem snd_mix
  snd_sdl sv_main sv_move sv_phys sv_user sys_sdl view wad world zone
  in_evdev r_part updater
  gl_draw gl_mesh gl_model gl_refrag gl_rlight gl_rmain gl_rmisc gl_rsurf
  gl_screen gl_warp gl_vidsdl gles_compat
"

mkdir -p "$BUILDDIR"
echo "=== GL Quake webOS build (OpenGL ES 1.1, 1024x768) ==="
echo "CC: $CC ($($CC -dumpversion))"

compile_c() {
    local src="$1" obj="$2" lvl
    for lvl in "$OPTS" "-O1 -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp" "-O0"; do
        if $CC ${CFLAGS/$OPTS/$lvl} -c "$src" -o "$obj" 2>/tmp/cc.err; then
            [ "$lvl" = "$OPTS" ] || echo "    (fell back to: $lvl)"
            return 0
        fi
        grep -q "internal compiler error" /tmp/cc.err || { cat /tmp/cc.err >&2; return 1; }
    done
    echo "ERROR: $src fails even at -O0" >&2; cat /tmp/cc.err >&2; return 1
}

# Newest of everything that is NOT a .c file but still changes the output: any
# header, this script (it sets CFLAGS), and appinfo.json (it supplies
# -DQUAKEHD_VERSION_RAW). Without this, editing updater.h or bumping the
# version reuses stale objects and silently ships the previous build.
NEWEST_DEP=$(ls -t "$SRC"/*.h "$0" "$SRC/../build/webos/hd/appinfo.json" 2>/dev/null | head -1)

OBJECTS=""
for s in $C_SRCS; do
    src="$SRC/$s.c"
    obj="$BUILDDIR/$s.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || \
       { [ -n "$NEWEST_DEP" ] && [ "$NEWEST_DEP" -nt "$obj" ]; }; then
        echo "CC  $s.c"
        compile_c "$src" "$obj"
    fi
    OBJECTS="$OBJECTS $obj"
done

echo "Linking..."
$CC $OBJECTS $LDFLAGS -o "$OUT"
echo ""
echo "Build complete: $OUT"
ls -lh "$OUT"
readelf -V "$OUT" | grep -o 'GLIBC_[0-9.]*' | sort -u | sed 's/^/  needs /'
