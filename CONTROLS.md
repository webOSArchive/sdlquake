# Quake TouchPad — Controller Mapping (reference)

The editable mapping lives in **`CONTROLS.csv`**. This file is the reference: the
verified hardware truth tables, how the columns work, the implemented scheme, and
the action vocabulary.

---

## How it works now (implemented in 1.4.4, verified on device E)

Decoding happens in **two stages**, so one scheme fits every pad:

1. A per-pad **profile** (`padprofile_t` in `src/in_evdev.c`, matched on the
   evdev device name) maps that pad's raw evdev codes and axes onto
   pad-independent **physical positions** — the CONTROLS.csv columns.
2. **One action scheme** (`pad_recompute`) binds those positions to Quake
   actions.

Adding a controller is therefore a new profile table entry and nothing else.
Profiles shipped: **DS4** (BT shim + USB), **ShanWan/Xbox knock-off**,
**Logitech Precision**, and a **generic** fallback covering both the standard
`BTN_` gamepad block and the `BTN_JOYSTICK` block.

This replaced code that hard-coded the DS4 shim's button order as if it were
universal — which is exactly why every other pad felt "unpredictable".

### The implemented scheme

**In game:** face buttons LOOK (left/right turn, up/down pitch — so aiming works
on pads with no right stick); left stick + d-pad MOVE+STRAFE; right stick LOOKs;
shoulders JUMP (L) / FIRE (R); triggers RUN (L) / NEXT WEAPON (R), read from
their *analog* value at 30% pull; stick clicks RUN; Start opens the MENU; Back
selects the PREVIOUS weapon.

**In menu:** d-pad / left stick NAVIGATE, and the pad splits down the middle —
everything on the **right** side SELECTS (face-down, face-right, R
shoulder/trigger/click), everything on the **left** side goes BACK (face-left,
face-up, L shoulder/trigger/click, Back, Start). No in-game action fires while a
menu is open.

`CONTROLS.csv` now records this as the single authoritative `SCHEME` pair of
rows, plus a per-pad `cheatsheet` in that pad's own printed button names. The
earlier per-pad `DESIRED` rows are gone because they contradicted each other —
the Xbox row asked for Start = Select and right-trigger = Back where the DS4,
Logitech and Saturn rows asked for the opposite. Inconsistency between pads was
the complaint this rewrite existed to fix, so the majority won: **right side
selects, left side backs out**, and Start closes a menu the same way it opens
one. Left-stick menu navigation and Back = PrevWpn were blank in the CSV and are
additions; without the latter there is no way to cycle weapons backwards.

---

## How the CSV columns work

Columns are **physical positions**, using DS4 names as the canonical labels:

- `Square` = face-left, `Cross` = face-down, `Circle` = face-right,
  `Triangle` = face-up
- `L1/R1` = top shoulders, `L2/R2` = lower triggers
- `Share`/`Options`/`PS` = select / start / guide, `Touchpad` = DS4 touch click
- `L3/R3` = stick clicks, `LeftStick`/`RightStick` = analog sticks, `Dpad` = hat

Every gamepad has these positions, so **one positional scheme works for all pads**.
Per pad, I capture the position→evdev-code table separately (DS4 is done below;
I can capture the Logitech / Saturn / Xbox pads the same way on request). You only
need to decide the *actions*; I handle which evdev code that is on each pad.

`Row` values: `CURRENT` = what it does today, `DESIRED` = what you want,
`reference` = the evdev index row (don't edit).

---

## DS4 — verified hardware truth table (captured on device E, 2026-07-29)

Read directly from the pad's evdev node (grabbed). `index = evdev code − 0x130`.

| Physical | code | index | | Physical | evdev axis |
|----------|------|:-----:|-|----------|-----------|
| ■ Square   | 0x130 | 0  | | Left stick  | ABS_X / ABS_Y   |
| ✕ Cross    | 0x131 | 1  | | Right stick | ABS_Z / ABS_RZ  |
| ● Circle   | 0x132 | 2  | | L2 (analog) | ABS_RX          |
| ▲ Triangle | 0x133 | 3  | | R2 (analog) | ABS_RY          |
| L1         | 0x134 | 4  | | D-pad       | ABS_HAT0X/Y     |
| R1         | 0x135 | 5  | |
| L2         | 0x136 | 6  | | *Left stick rests ~124–125 (jitter);*
| R2         | 0x137 | 7  | | *the 20% dead-zone covers it.*
| Share      | 0x138 | 8  | |
| Options    | 0x139 | 9  | |
| L3 (click) | 0x13a | 10 | |
| R3 (click) | 0x13b | 11 | |
| PS         | 0x13c | 12 | |
| Touchpad   | 0x13d | 13 | |

The L2/R2 **analog** values land on ABS_RX/RY (they also send a digital press);
they rest at an extreme so movement code correctly ignores them.

**Trigger handling (why Fire/Jump are on the shoulders, not the triggers).**
The analog triggers only send their *digital* click near a **full** pull, so a
light or slow pull is missed and rapid-fire is impossible — bad for twitch
actions. So the twitchy actions (**Fire, Jump**) live on the crisp **digital
shoulders (L1/R1 · LB/RB)**, and the **triggers (L2/R2 · LT/RT)** do the
non-twitch actions (**Run, Next-weapon**), read from their **analog** value at
~30 % pull so a light/slow pull still registers. Same pattern on any pad with a
bumper+trigger pair (DS4, Xbox). This is why the CSV shows shoulders =
Jump/Fire and triggers = Run/NextWpn.

---

## ShanWan "Xbox knock-off" — verified truth table (device E, 2026-07-29)

Captured by grabbing `/dev/input/event3` and pressing one control at a time
(`build/webos/evread.c`). Name `ShanWan Wireless Gamepad`, VID:PID `0079:181c`.

It advertises **16** contiguous buttons (`0x130`–`0x13f`) but only uses 13, and
the used codes are **not** sequential from `0x130` — `0x132` and `0x135` are
skipped. That gap pattern is the DragonRise family's shared descriptor (VID
`0079` is DragonRise, the same vendor as the Saturn-style pad), where the face
buttons occupy the A/B/[C]/X/Y/[Z] slots. So the advertised bitmap alone cannot
tell you the layout — only a labelled capture can.

| Physical | code | | Physical | code | | Axis | evdev |
|---|---|-|---|---|-|---|---|
| A (face down)  | `0x130` | | Back  | `0x13a` | | Left stick  | `ABS_X` / `ABS_Y` |
| B (face right) | `0x131` | | Start | `0x13b` | | Right stick | `ABS_Z` / `ABS_RZ` |
| X (face left)  | `0x133` | | Guide | `0x13c` | | LT analog   | `ABS_BRAKE` (rests 0) |
| Y (face up)    | `0x134` | | LS click | `0x13d` | | RT analog | `ABS_GAS` (rests 0) |
| LB | `0x136` | | RS click | `0x13e` | | D-pad | `ABS_HAT0X/Y` (−1…1) |
| RB | `0x137` | | | | | | |
| LT digital | `0x138` | | | | | | |
| RT digital | `0x139` | | | | | | |

The analog triggers on `ABS_GAS`/`ABS_BRAKE` are the trap: the old code only ever
looked at `ABS_RX`/`ABS_RY` (where the *DS4* puts them), so on this pad the
triggers did nothing at all.

---

## DragonRise "Sega Saturn style" — verified truth table (device E, 2026-07-29)

VID:PID `0079:0011`, enumerates as `SWITCH CO.,LTD. USB Gamepad`. Ten buttons on
the generic `BTN_JOYSTICK` block, in the **Saturn console's own report order**
— X, A, B, Y, C, Z, then L, R, Select, Start — so the six face buttons
**interleave the two rows** rather than running row by row:

| Physical | code | | Physical | code |
|---|---|-|---|---|
| X (top-left)     | `0x120` | | L trigger | `0x126` |
| A (bottom-left)  | `0x121` | | R trigger | `0x127` |
| B (bottom-mid)   | `0x122` | | Select    | `0x128` |
| Y (top-mid)      | `0x123` | | Start     | `0x129` |
| C (bottom-right) | `0x124` | | | |
| Z (top-right)    | `0x125` | | | |

The 8-way d-pad is the **`ABS_RX`/`ABS_RY` pair** — `0`/`128`/`255` = left/centre/
right and up/centre/down, diagonals setting both. It is *not* a hat, and not the
`ABS_MISC` (range 0–7) axis the pad also advertises, which never fires. No analog
stick, no analog trigger; the d-pad drives Move+Strafe by reading as a stick at
full deflection, the same way the Logitech's `ABS_X/Y` d-pad does.

**Three phantom axes.** This pad advertises `ABS_X` permanently pegged at **1** of
0–255, plus `ABS_Y` and `ABS_Z` sitting dead centre and never moving. `ABS_X` is
what made the generic profile strafe the player into a wall forever, and is why
a stick role now requires a centre-resting axis (`stick_ok`); `ABS_Y`/`ABS_Z` are
why a profiled pad no longer lets the prober adopt stray axes (`adopt_axes`).
Two general lessons: **an advertised axis is not a real axis**, and a pad's
advertised button bitmap cannot tell you its layout — only a labelled capture can.

---

## Keyboard (USB / Bluetooth)

Physical keyboard keys already pass straight through as their normal Quake keys
(arrows, Enter, Esc, WASD, number keys, `~` console, …). If you want any remaps,
just list them here or in a note — they don't fit the gamepad CSV columns.

---

## Vocabulary — available ACTIONS (use these exact names in the CSV)

**Movement / look** (sticks, d-pad):
`Move+Strafe` (fwd/back + strafe L/R) · `Move+Turn` (fwd/back + turn L/R,
old-school) · `Look` (right-stick turn + pitch) · `Turn` (yaw only) · `Strafe` ·
`MoveFwd` · `MoveBack`

**Buttons:**
`Fire` · `Jump` · `SwimUp` · `SwimDown` · `Run` (hold = faster) · `NextWpn` ·
`PrevWpn` · `Wpn1`…`Wpn8` (specific weapon) · `Menu` (open/close = Esc) ·
`Console` (~) · `Select` (Enter, menus) · `Back` (Esc, menus) · `Scores`
(show frags) · `Pause` · `-` (unbound)

Need something not listed? Describe it and I'll wire it up.
