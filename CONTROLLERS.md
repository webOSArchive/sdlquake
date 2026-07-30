# Adding controller support

A guide for the next person — human or AI — who wants a gamepad this port has
never seen to work in Quake on a webOS TouchPad.

**Read this first: there are two routes, and most pads need neither.**

1. **Nothing at all.** An unknown pad falls back to a generic profile that
   follows the standard evdev conventions and auto-detects sticks, d-pads and
   analog triggers from how each axis *rests*. Plug it in and try it.
2. **Route A — remap in the game, no rebuild** (below). Four console commands.
   No toolchain, no Linux host, no source changes. **Try this before Route B.**
3. **Route B — add a permanent profile** (below). A table entry in
   `src/in_evdev.c`, so the pad works out of the box for everybody.

Route A is also how you *discover* what Route B should contain, so they compose:
map the pad live, confirm it plays well, then write the profile down.

---

## The model you are working inside

Pads disagree wildly about which evdev code a physical button sends. There is no
standard to rely on — this port has measured four pads and found **four
different layouts**, including two that both use the `0x130` block but assign it
differently. So decoding is deliberately split in two:

```
raw evdev codes ──(per-pad PROFILE)──▶ physical POSITIONS ──(one ACTION SCHEME)──▶ Quake
```

* A **profile** knows only *this pad's* wiring: which code is the face-left
  button, which axis is the left stick, which axis is the left trigger.
* The **action scheme** (`pad_recompute` in `src/in_evdev.c`) knows only
  positions, never codes. It is the same for every pad.

**Adding a pad therefore means adding a profile. Never edit the action scheme**
— that would change behaviour for every other controller, which is exactly the
inconsistency this design exists to prevent.

### The positions

These are the columns in `CONTROLS.csv`, named after the DS4 because it is the
most familiar layout.

| Position | DS4 | Xbox-style | Generic pad |
|---|---|---|---|
| `face_left` | Square | X | button 1 |
| `face_down` | Cross | A | button 2 |
| `face_right` | Circle | B | button 3 |
| `face_up` | Triangle | Y | button 4 |
| `shoulder_l` | L1 | LB | button 5 |
| `shoulder_r` | R1 | RB | button 6 |
| `trigger_l` | L2 | LT | button 7 |
| `trigger_r` | R2 | RT | button 8 |
| `back` | Share | Back | button 9 |
| `start` | Options | Start | button 10 |
| `guide` | PS | Guide | — |
| `click_l` / `click_r` | L3 / R3 | stick clicks | — |
| `extra` | touchpad click | — | — |

### What each position does

| Position | In game | In a menu |
|---|---|---|
| four face buttons | Look left / down / right / up | left+up = Back, down+right = Select |
| `shoulder_l` | Jump | Back |
| `shoulder_r` | Fire | Select |
| `trigger_l` | Run (hold) | Back |
| `trigger_r` | Next weapon | Select |
| `click_l` / `click_r` | Run | Back / Select |
| `back` | Previous weapon | Back |
| `start` | Open menu | Back |
| `guide`, `extra` | unbound | unbound |
| left stick, d-pad | Move + strafe | Navigate |
| right stick | Look | — |

Two deliberate design points, both learned the hard way:

* **The face buttons aim.** Many pads have no right stick, and a pad you cannot
  aim with is unplayable. Putting look on the face buttons means every pad can
  aim, and pads *with* a right stick simply get both.
* **Triggers are read from their analog value at 30% pull**, not their digital
  click. A trigger's digital half only fires near a *full* pull, which makes the
  action feel broken. `trigger_*` positions still work as plain buttons on pads
  whose triggers are digital only.

---

## Route A — map a pad from inside the game

Four console commands. Open the console with **`** (backtick) on a keyboard, or
from the touch overlay.

| Command | What it does |
|---|---|
| `padstatus` | What pad is open, which profile matched, the axis roles in force, and every button code the pad reports with the position it currently maps to |
| `padtest` | Toggle. Echoes every raw button and axis event to the console as you press things |
| `padbtn <code> <position>` | Bind an evdev code to a position |
| `padaxis <role> <axis>` | Bind an axis role to an ABS code (`-1` disables) |

Axis roles: `move_x`, `move_y`, `look_x`, `look_y`, `trig_l`, `trig_r`,
`dpad_x`, `dpad_y`. Common ABS codes: `x=0 y=1 z=2 rx=3 ry=4 rz=5 gas=9
brake=10`; the hat (`ABS_HAT0X/Y`) is always used automatically and needs no
role.

### The workflow

```
padstatus              # see what was detected and guessed
padtest                # turn on the echo
                       # press each control in turn and read off its code
padtest                # turn the echo back off
padbtn 0x131 face_right
padbtn 0x136 shoulder_l
padaxis look_x 3
padstatus              # confirm
```

Then make it permanent by putting the `padbtn`/`padaxis` lines in
**`id1/autoexec.cfg`** (create the file if it does not exist). Quake execs that
at startup, so the mapping applies on every launch and survives unplugging and
replugging the pad.

```
// id1/autoexec.cfg -- my no-name gamepad
padbtn 0x131 face_right
padbtn 0x136 shoulder_l
padaxis look_x 3
padaxis look_y 4
```

Overrides always win over the profile and over auto-detection, so you can
correct a partly-wrong guess without fighting it.

---

## Route B — add a permanent profile

Do this once Route A tells you the pad's real layout, so it works for everyone
with no configuration.

### 1. Capture the truth table

`build/webos/evread.c` is a small standalone probe. Build it with the PDK
toolchain (**not** a modern cross-compiler — see the traps), push it, run it:

```sh
/opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-gcc -O2 -o evread evread.c
novacom -d usb put file:///media/internal/evread < evread
# on the device:
chmod +x /media/internal/evread
/media/internal/evread /dev/input/event3
```

It prints the device name, every `EV_KEY` code the pad advertises, and every
`EV_ABS` axis with its min, max and **resting value** — then streams named
events as you press things.

The resting value is the single most useful number it reports. It is what tells
a stick or d-pad axis (rests at centre) from an analog trigger (rests at one
end) from a phantom axis that does not exist (see the traps).

Press **one control at a time**, in a written-down order, and record which code
each produced. You cannot skip this and infer the layout from the advertised
code list — see trap 1.

### 2. Write the profile

In `src/in_evdev.c`, next to the existing ones:

```c
/* ------------------------------------------------------------------------
 * <Pad name> (<VID:PID>), enumerates as "<exact evdev name>".
 * Captured on <device> <date>. <Anything odd about it.>
 * ---------------------------------------------------------------------- */
static const btnmap_t btns_mypad[] = {
    { 0x130, P_FACE_DOWN  },   /* A */
    { 0x131, P_FACE_RIGHT },   /* B */
    /* ... one line per button the pad actually has ... */
    { 0, P_NONE }              /* terminator -- required */
};
static const padprofile_t prof_mypad = {
    "Substring Of The Name",   /* matched with strstr() against the evdev name */
    "MyPad",                   /* short label for the console line */
    btns_mypad,
    ABS_X,    ABS_Y,           /* move_x, move_y  -- left stick, or the d-pad */
    ABS_Z,    ABS_RZ,          /* look_x, look_y  -- right stick, or AX_NONE  */
    ABS_BRAKE, ABS_GAS,        /* trig_l, trig_r  -- analog triggers, or AX_NONE */
    AX_NONE,  AX_NONE,         /* dpad_x, dpad_y  -- only if not on the hat   */
    0                          /* adopt_axes: 0 for a known pad. See trap 3.  */
};
```

Then add it to the `profiles[]` array **before `&prof_default`** (which must
stay last, as the catch-all):

```c
static const padprofile_t *profiles[] = {
    &prof_ds4, &prof_shanwan, &prof_logitech, &prof_dragonrise,
    &prof_mypad,
    &prof_default
};
```

Notes that will save you time:

* The button list is `{code, position}` **pairs, not an indexed array**. Do not
  "simplify" it to designated initializers (`[0x130] = P_FACE_DOWN`): unlisted
  entries would default to `0`, and every unmapped code would silently alias to
  a real position. `P_NONE` is deliberately `0` to make the pair list safe.
* Match on the **shortest distinctive substring** of the name. Names contain
  trailing spaces and vendor noise (`"SWITCH CO.,LTD. USB Gamepad "`).
* On a pad with **no analog stick**, point `move_x`/`move_y` at the d-pad's axis
  pair. It reads as a stick at full deflection, which is exactly right.

### 3. Verify, then write it down

Launch the game and read the two log lines it prints when the pad opens:

```
evdev: gamepad '<name>' on /dev/input/event3 -- profile MyPad (rstick=1)
evdev:  move=0/1 look=2/5 trig=10/9 dpad=-1/-1
```

`-1` means "that role is unavailable". If something you expected shows `-1`, the
axis is either unmapped, absent, or was rejected for not resting at centre.

Finally add the pad to `CONTROLS.csv` (an `evdev-code` reference row and the two
`cheatsheet` rows) and to the verified list in `README`. `CONTROLS.md` holds the
per-pad truth tables in full.

---

## Traps

Every one of these cost real debugging time. They are listed by how likely they
are to bite you.

**1. A pad's advertised button list does not tell you its layout.** The ShanWan
advertises 16 contiguous codes (`0x130`–`0x13f`) but uses 13, skipping `0x132`
and `0x135`. The DS4-via-Bluetooth-shim advertises 14 contiguous codes and packs
its *report order* into them, so `0x134` is L1 — while on the ShanWan `0x134` is
a face button. Both look like "the `0x130` block". **Only a labelled capture,
where a human presses known buttons in a known order, resolves this.**

**2. An advertised axis is not a real axis.** The DragonRise advertises `ABS_X`
permanently pegged at `1` of `0–255` while all its genuine axes rest at `128`.
Read as a stick, that is full deflection forever: the player strafes into a wall
and cannot stop. This is now guarded — a stick role only accepts a
centre-resting axis (`axis_t.stick_ok`) — but if a pad behaves oddly, check the
resting values first. The same pad also advertises two more axes that never
move, and an `ABS_MISC` that does nothing.

**3. Do not let auto-detection loose on a pad you have a table for.** The
prober adopts unclaimed centre-resting axes as a d-pad, which is useful for an
unknown pad and harmful for a known one — it would have wired the DragonRise's
phantom axes into movement. Hence `adopt_axes`, which is `1` only for
`prof_default`. Set it to `0` in your profile.

**4. Narrow axes and noise gates do not mix.** A "report only if it moved 25% of
its range" filter silently swallows *every* event from an axis whose whole range
is `-1..1` (a hat) or `0..7` (an 8-way direction code). This ate a d-pad twice
before being fixed. If a control appears completely dead in a capture, suspect
the gate before the hardware.

**5. A hotplugged pad may hand you its node before its axis info exists.** Open
it within a second of appearing and `EVIOCGBIT(EV_ABS)` can fail, leaving a pad
held with no axes and therefore no movement, forever. `axes_setup()` now returns
a count and is re-run for a few seconds when it finds nothing. If you change
that function, keep it safe to call twice.

**6. Wireless pads sleep, and their dongle does not.** The USB dongle stays
enumerated, so the input node never disappears and a capture just sits there
empty while you assume your code is broken. Wake the pad and work briskly.

**7. `EVIOCGRAB` failing with `EBUSY` means a shared reader gets nothing.** If
Quake is already running it holds the grab, so your probe captures silence. Stop
the game first. Any other errno and shared reading still works.

**8. Build with the PDK's own toolchain.**
`/opt/PalmPDK/arm-gcc/bin/arm-none-linux-gnueabi-gcc`. The TouchPad has glibc
2.8; a modern cross-compiler links against `GLIBC_2.15` and the binary will not
load at all. Verify with `readelf -V <binary> | grep GLIBC` — everything must be
2.8 or lower. Also: do not pass `-funroll-loops`, which makes this gcc crash.

**9. The app jail hides `/dev/input`.** A pad works from a shell and does
nothing when launched from the launcher unless the package's `postinst`
bind-mounts `/dev/input` into the jail and makes the nodes readable by uid 5003.
Quake is `"type":"game"`, so the mount belongs in `/etc/jail_game.conf` — not
`jail_pdk.conf`. Check what a running, jailed process actually sees with
`ls /proc/<pid>/root/dev/input/`.

---

## Files

| File | What it is |
|---|---|
| `src/in_evdev.c` | Everything: profiles, decoding, action scheme, console commands |
| `src/in_evdev.h` | The four entry points, called from `src/vid_sdl.c` |
| `build/webos/evread.c` | Standalone capture probe |
| `CONTROLS.csv` | Machine-readable map: truth tables and per-pad cheatsheets |
| `CONTROLS.md` | Per-pad truth tables in full, with the reasoning |
| `README` | Player-facing control reference |
