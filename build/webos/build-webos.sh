#!/bin/bash
# Build SDL Quake for the HP webOS TouchPad (ARM Cortex-A8).
# Uses the HP webOS PDK toolchain + its bundled SDL/SDL_image/PDL.
#
# Adds direct-evdev USB/Bluetooth controller + keyboard support (in_evdev.c) and
# the menu-aware touch overlay -- see src/in_evdev.c and src/vid_sdl.c.

set -e

PDK="/opt/PalmPDK"
# MUST use the PDK's own CodeSourcery gcc: it targets the device's ancient glibc
# (GLIBC_2.4; the TouchPad has glibc 2.8). The Linaro 4.9 toolchain links libm
# against GLIBC_2.15 and the resulting binary won't even load
# ("version `GLIBC_2.15' not found"). The PDK gcc's only quirk is an internal
# compiler error with -funroll-loops -- which we simply don't pass (plain -O2 is
# fine); a per-file -O1/-O0 fallback below covers any other ICE.
TOOLCHAIN_BIN="$PDK/arm-gcc/bin"
CC="$TOOLCHAIN_BIN/arm-none-linux-gnueabi-gcc"

SRC="../../src"
BUILDDIR="fbuild/webos"
OUT="$BUILDDIR/sdlquake.bin"

# Match the ORIGINAL (known-good) build's flags exactly, minus only
# -funroll-loops (which ICEs this gcc). CRITICAL: do NOT add -fsigned-char --
# the original built with ARM's default UNSIGNED char, and Quake has
# char-indexed tables; flipping to signed char turns high bytes negative and
# corrupts the heap (it crashed in malloc during early file parsing, but only
# when jailed, because the corruption is data/path-dependent).
OPTS="-O2 -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp -ffast-math -fsingle-precision-constant"
CFLAGS="$OPTS -D__webos__ -DSDL -DELF -D_GNU_SOURCE=1 -D_REENTRANT"
CFLAGS="$CFLAGS -I$PDK/include -I$PDK/include/SDL -I$SRC"

LDFLAGS="-L$PDK/device/lib -Wl,-rpath,/usr/local/lib -lSDL -lSDL_image -lpdl -lpthread -lm"

# C sources (the SDL target's object list from the Makefile) + our new module.
C_SRCS="
  cd_sdl chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console
  crc cvar d_edge d_fill d_init d_modech d_part d_polyse d_scan d_sky d_sprite
  d_surf d_zpoint draw host host_cmd keys mathlib menu model net_bsd net_dgrm
  net_loop net_main net_udp net_vcr net_wso pr_cmds pr_edict pr_exec r_aclip
  r_alias r_bsp r_draw r_edge r_efrag r_light r_main r_misc r_part r_sky
  r_sprite r_surf r_vars sbar screen snd_dma snd_mem snd_mix snd_sdl sv_main
  sv_move sv_phys sv_user sys_sdl vid_sdl view wad world zone d_vars nonintel
  in_evdev
"

# x86 asm stubs -- guarded by '#if id386', which is 0 on ARM, so they assemble
# to empty objects (their C fallbacks above do the real work). Compiled for
# object-file parity with the original Makefile.
ASM_SRCS="
  r_varsa snd_mixa sys_dosa d_draw d_draw16 d_polysa d_scana d_spr8 d_varsa
  math r_aclipa r_aliasa r_drawa r_edgea surf16 surf8 worlda
"

mkdir -p "$BUILDDIR"
echo "=== SDL Quake webOS build ==="
echo "CC: $CC ($($CC -dumpversion))"

# Compile one C file, falling back -O2 -> -O1 -> -O0 if this old gcc ICEs on it.
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

OBJECTS=""
for s in $C_SRCS; do
    src="$SRC/$s.c"
    obj="$BUILDDIR/$s.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        echo "CC  $s.c"
        compile_c "$src" "$obj"
    fi
    OBJECTS="$OBJECTS $obj"
done

for s in $ASM_SRCS; do
    src="$SRC/$s.S"
    obj="$BUILDDIR/$s.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        echo "AS  $s.S"
        $CC $CFLAGS -c "$src" -o "$obj"
    fi
    OBJECTS="$OBJECTS $obj"
done

echo "Linking..."
$CC $OBJECTS $LDFLAGS -o "$OUT"
echo ""
echo "Build complete: $OUT"
ls -lh "$OUT"
file "$OUT"
