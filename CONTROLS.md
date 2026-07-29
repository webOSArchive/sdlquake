# Quake TouchPad — Controller Mapping (reference)

The editable mapping lives in **`CONTROLS.csv`** — fill in the `DESIRED` rows
there (add rows/columns freely). This file is just the reference: the verified
hardware truth table, how the columns work, current behaviour, and the action
vocabulary. Use the exact action names from the **vocabulary** so each binding is
unambiguous to implement. When the CSV is filled, I implement every scheme in one
pass and you test each pad.

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

## Current behaviour (the "unpredictable" bits to fix)

- **Weapon-switch and Run stay live in menus** — L1/R1/L2 keep acting in the
  background while a menu is open. (Marked "(x)" in the CSV.)
- **Context-switching**: ✕ and ● change meaning between game and menu.
- **Shared actions**: Fire = Square + Cross + R2; Jump = Circle + Triangle.

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
