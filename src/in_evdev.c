// in_evdev.c -- webOS direct-evdev input: USB & Bluetooth game controllers
// and physical (USB/BT) keyboards, injected straight into Quake's Key_Event().
//
// Why this exists (three webOS 3.0.5 platform facts, see the WOSA "game
// controllers" knowledge doc):
//   1. There is NO joydev -- SDL_INIT_JOYSTICK finds nothing. Pads appear only
//      as /dev/input/eventN. So we read evdev directly.
//   2. webOS routes physical keyboards through hidd, and the PDK/SDL webOS key
//      path is unreliable for external keyboards. Reading the keyboard's evdev
//      node ourselves (and EVIOCGRAB-ing it) guarantees keys reach the game
//      regardless, with no double-input.
//   3. The PDK app jail hides /dev/input; the package's postinst bind-mounts it
//      in and 0666's the nodes (uid 5003). Without that this module opens
//      nothing -- which is exactly why a game works from a shell but not the
//      launcher.
//
// The controller decode (capability detection, flexible axis probing, DS4 +
// generic pads) is the model proven end-to-end on this hardware in Commander
// Keen (com.cmdrkeen.game); here it drives Quake keys + analog move/look.

#include "in_evdev.h"

#ifdef __webos__

#include "SDL.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
// quakedef.h (via in_evdev.h) already pulls in keys.h, client.h, cvar.h, etc.

// The PDK's kernel headers predate the BTN_SOUTH gamepad naming; fall back to
// the classic codes (all identical values).
#ifndef BTN_SOUTH
#define BTN_SOUTH    0x130   // == BTN_A / BTN_GAMEPAD
#endif
#ifndef BTN_GAMEPAD
#define BTN_GAMEPAD  0x130
#endif
#ifndef BTN_JOYSTICK
#define BTN_JOYSTICK 0x120
#endif

// --- Quake globals we drive/read ------------------------------------------
extern int       in_impulse;
extern double    host_frametime;
extern cvar_t    cl_forwardspeed, cl_sidespeed, cl_yawspeed, cl_pitchspeed;
void Key_Event (int key, qboolean down);
void V_StartPitchDrift (void);   // lookspring: recenter pitch to level
void V_StopPitchDrift (void);    // hold pitch while actively looking

// ==========================================================================
//  Device table
// ==========================================================================
#define MAX_DEVS   8
#define DEV_NONE   0
#define DEV_PAD    1
#define DEV_KBD    2

typedef struct {
    int  fd;
    int  kind;      // DEV_PAD / DEV_KBD
    int  idx;       // /dev/input/eventN index we hold (-1 = free slot)
} evdev_t;

static evdev_t devs[MAX_DEVS];
static Uint32  last_scan;
static int     scan_verbose = 3;   // chatty right after a disconnect, then quiet

// ==========================================================================
//  Gamepad state
//    DS4 button indices are the ones VERIFIED on this hardware in Commander
//    Keen -- the shim emits its HID report order onto BTN_SOUTH+i, which is
//    NOT the standard BTN_ ordering, so trust these, not <linux/input.h>.
// ==========================================================================
// Verified on device E 2026-07-29 by grabbing the pad and pressing each button
// (see CONTROLS.md): contiguous index = code - 0x130.
#define GP_SQUARE   0   // face left
#define GP_CROSS    1   // face down
#define GP_CIRCLE   2   // face right
#define GP_TRIANGLE 3   // face up
#define GP_L1       4
#define GP_R1       5
#define GP_L2       6
#define GP_R2       7
#define GP_SHARE    8
#define GP_OPTIONS  9
#define GP_L3       10
#define GP_R3       11
#define GP_PS       12
#define GP_TOUCHPAD 13
#define GP_NBTN     16
#define GP_NGEN     16          // generic pad buttons at BTN_JOYSTICK(0x120)+

static byte gp_btn[GP_NBTN];   // DS4 layout,     index = code - BTN_SOUTH
static byte gp_gbtn[GP_NGEN];  // generic layout, index = code - BTN_JOYSTICK

// Analog sticks: left = ABS_X/ABS_Y, right = ABS_Z/ABS_RZ (DS4 convention).
// A d-pad reported on X/Y (Logitech-style 0/128/255) rides these too -- it just
// reads as full-tilt. Each entry caches EVIOCGABS center/half-range so non
// 0-255 pads work.
typedef struct { int have, center, half, val; } stickaxis_t;
static stickaxis_t ax_lx, ax_ly, ax_rx, ax_ry;

// Digital d-pad voters: the hat, plus ABS_RX/RY when they REST near center
// (that catches a Saturn-style pad whose d-pad sits on RX/RY, while excluding a
// DS4's analog triggers on RX/RY, which rest at an extreme). Feeds menu arrows
// and movement on pads that have no real stick.
static int rxry_on[2];                 // [0]=RX, [1]=RY kept as a d-pad axis?
static int rxry_center[2], rxry_dead[2], rxry_val[2];
// On a DS4, ABS_RX/RY are the L2/R2 ANALOG triggers (rest at min, so rxry_on=0).
// We read them as triggers with a low pull threshold so a light/slow pull counts
// -- the digital BTN only clicks near a full pull, which makes fire/quick actions
// feel unresponsive. Min/max come from the axis's own range.
static int rxry_have[2], rxry_min[2], rxry_max[2];
static int hatx = 0, haty = 0;

// Derived, recomputed each frame:
static float mv_fwd, mv_side, look_yaw, look_pitch;   // -1..1
static int   dig_left, dig_right, dig_up, dig_down;   // menu arrows
static int   have_right_stick;

// Rising-edge trackers for one-shot impulses (weapon switch).
static int prev_nextweap, prev_prevweap;

// ==========================================================================
//  Quake key reconciliation
//    Pad-driven keys are rebuilt into want[] every frame and diffed against
//    have[]; multiple sources for one key never race on release, and switching
//    key_dest cleanly drops keys that no longer apply (e.g. menu arrows).
// ==========================================================================
#define QK_MAX 256
static byte qk_want[QK_MAX];
static byte qk_have[QK_MAX];

static void qk_mark(int key)   { if (key > 0 && key < QK_MAX) qk_want[key] = 1; }

static void qk_reconcile(void)
{
    int k;
    for (k = 0; k < QK_MAX; k++) {
        if (qk_want[k] != qk_have[k]) {
            qk_have[k] = qk_want[k];
            Key_Event(k, qk_want[k]);
        }
    }
}

static void qk_release_all(void)
{
    int k;
    for (k = 0; k < QK_MAX; k++)
        if (qk_have[k]) { qk_have[k] = 0; Key_Event(k, 0); }
}

// ==========================================================================
//  Gamepad open / probe
// ==========================================================================
static int has_key_cap(int fd, int code)
{
    unsigned long bits[(KEY_MAX / (8 * sizeof(long))) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return 0;
    return (bits[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1UL;
}

// A keyboard-ish device: advertises letter keys but is NOT a pad. (We also skip
// the TouchPad's built-in button nodes by name in dev_open.)
static int looks_like_keyboard(int fd)
{
    return has_key_cap(fd, KEY_A) && has_key_cap(fd, KEY_Z) &&
           has_key_cap(fd, KEY_ENTER) && !has_key_cap(fd, BTN_GAMEPAD) &&
           !has_key_cap(fd, BTN_JOYSTICK);
}

static void stick_setup(int fd, int code, stickaxis_t *s)
{
    struct input_absinfo ai;
    s->have = 0; s->center = 128; s->half = 128; s->val = 128;
    if (ioctl(fd, EVIOCGABS(code), &ai) != 0) return;
    if (ai.maximum <= ai.minimum) return;
    s->have   = 1;
    s->center = (ai.minimum + ai.maximum) / 2;
    s->half   = (ai.maximum - ai.minimum) / 2;
    if (s->half < 1) s->half = 1;
    s->val    = ai.value;
}

// ABS_RX / ABS_RY as digital d-pad axes -- kept only if center-resting.
static void rxry_setup(int fd)
{
    struct input_absinfo ai;
    int i;
    const int codes[2] = { ABS_RX, ABS_RY };
    for (i = 0; i < 2; i++) {
        int center, quarter;
        rxry_on[i] = 0; rxry_center[i] = 128; rxry_dead[i] = 56; rxry_val[i] = 128;
        rxry_have[i] = 0; rxry_min[i] = 0; rxry_max[i] = 255;
        if (ioctl(fd, EVIOCGABS(codes[i]), &ai) != 0) continue;
        if (ai.maximum <= ai.minimum) continue;
        rxry_have[i] = 1;                    // axis exists (trigger or d-pad axis)
        rxry_min[i] = ai.minimum; rxry_max[i] = ai.maximum;
        rxry_val[i] = ai.value;
        center  = (ai.minimum + ai.maximum) / 2;
        quarter = (ai.maximum - ai.minimum) / 4;
        if (quarter < 1) quarter = 1;
        if (ai.value < center - quarter || ai.value > center + quarter)
            continue;                       // rests off-center -> trigger, not a d-pad
        rxry_on[i] = 1; rxry_center[i] = center; rxry_dead[i] = quarter;
    }
}

// Is analog trigger i (0=L2/RX, 1=R2/RY) pulled past ~30%? Only meaningful when
// the axis exists and is NOT being used as a centering d-pad axis.
static int trig_pulled(int i)
{
    int thresh;
    if (!rxry_have[i] || rxry_on[i]) return 0;
    thresh = rxry_min[i] + (rxry_max[i] - rxry_min[i]) * 30 / 100;
    return rxry_val[i] > thresh;
}

static void pad_setup_axes(int fd)
{
    stick_setup(fd, ABS_X,  &ax_lx);
    stick_setup(fd, ABS_Y,  &ax_ly);
    stick_setup(fd, ABS_Z,  &ax_rx);
    stick_setup(fd, ABS_RZ, &ax_ry);
    rxry_setup(fd);
    have_right_stick = ax_rx.have && ax_ry.have;
}

static void pad_reset_state(void)
{
    memset(gp_btn, 0, sizeof(gp_btn));
    memset(gp_gbtn, 0, sizeof(gp_gbtn));
    ax_lx.val = ax_lx.center; ax_ly.val = ax_ly.center;
    ax_rx.val = ax_rx.center; ax_ry.val = ax_ry.center;
    rxry_val[0] = rxry_center[0]; rxry_val[1] = rxry_center[1];
    hatx = haty = 0;
    prev_nextweap = prev_prevweap = 0;
}

// ==========================================================================
//  Scan for new devices (~1/s). Grabs pads AND keyboards exclusively so the
//  system doesn't also process them (no double input). The touch panel is NOT
//  on /dev/input here, so grabbing is safe -- we only skip the three built-in
//  button nodes by name.
// ==========================================================================
static int dev_is_open(const char *targetname_ignored) { (void)targetname_ignored; return 0; }

static void scan_devices(void)
{
    char path[32], name[80];
    int i, s, one = 1, seen = 0;
    int log = (scan_verbose > 0);

    // Re-scan event0..15, opening only nodes we DON'T already hold. Skipping
    // held nodes matters: re-opening + re-grabbing an active Bluetooth HID node
    // every second churns the BT stack and stalls the frame. We remember each
    // held node's index, so a held device is never touched by the scan again.
    for (i = 0; i < 16; i++) {
        int fd, kind, slot = -1, held = 0;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);

        for (s = 0; s < MAX_DEVS; s++)
            if (devs[s].fd >= 0 && devs[s].idx == i) { held = 1; break; }
        if (held) { seen++; continue; }

        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            /* ENOENT = node absent (pad asleep); EACCES = a root-only built-in
             * node (gpio-keys etc.) we intentionally can't read. Both expected. */
            if (log && errno != ENOENT && errno != EACCES)
                Con_Printf("evdev: open %s: %s\n", path, strerror(errno));
            continue;
        }
        seen++;
        name[0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        // Never grab the TouchPad's built-in buttons / power / headset keys.
        if (!strcmp(name, "gpio-keys") || !strcmp(name, "pmic8058_pwrkey") ||
            !strcmp(name, "headset")) { close(fd); continue; }

        if (has_key_cap(fd, BTN_GAMEPAD) || has_key_cap(fd, BTN_JOYSTICK))
            kind = DEV_PAD;
        else if (looks_like_keyboard(fd))
            kind = DEV_KBD;
        else { close(fd); continue; }

        // Grab exclusively so the system doesn't also process the pad. Best-
        // effort (like Commander Keen): if the grab fails, keep the fd and read
        // shared -- webOS ignores a pad's BTN_*/ABS_* codes, so no double-input.
        if (ioctl(fd, EVIOCGRAB, &one) != 0)
            Con_Printf("evdev: grab %s failed: %s (reading shared)\n",
                       path, strerror(errno));

        // Find a free slot.
        for (s = 0; s < MAX_DEVS; s++)
            if (devs[s].fd < 0) { slot = s; break; }
        if (slot < 0) { int zero = 0; ioctl(fd, EVIOCGRAB, &zero); close(fd); continue; }

        if (kind == DEV_PAD) {
            pad_setup_axes(fd);
            pad_reset_state();
            Con_Printf("evdev: gamepad '%s' on %s (rstick=%d)\n",
                       name, path, have_right_stick);
        } else {
            Con_Printf("evdev: keyboard '%s' on %s\n", name, path);
        }
        devs[slot].fd = fd;
        devs[slot].kind = kind;
        devs[slot].idx = i;
        scan_verbose = 3;
    }

    if (log && seen == 0) {
        Con_Printf("evdev: no controller/keyboard connected yet\n");
        scan_verbose--;
    }
}

// ==========================================================================
//  Keyboard: Linux KEY_* -> Quake key
// ==========================================================================
static int kbd_translate(int code, int *shifted_out)
{
    // returns Quake key (ascii or K_*), 0 if unmapped
    switch (code) {
        case KEY_ESC:        return K_ESCAPE;
        case KEY_ENTER:
        case KEY_KPENTER:    return K_ENTER;
        case KEY_TAB:        return K_TAB;
        case KEY_BACKSPACE:  return K_BACKSPACE;
        case KEY_DELETE:     return K_DEL;
        case KEY_INSERT:     return K_INS;
        case KEY_HOME:       return K_HOME;
        case KEY_END:        return K_END;
        case KEY_PAGEUP:     return K_PGUP;
        case KEY_PAGEDOWN:   return K_PGDN;
        case KEY_UP:         return K_UPARROW;
        case KEY_DOWN:       return K_DOWNARROW;
        case KEY_LEFT:       return K_LEFTARROW;
        case KEY_RIGHT:      return K_RIGHTARROW;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:   return K_ALT;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:  return K_CTRL;
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return K_SHIFT;
        case KEY_SPACE:      return K_SPACE;
        case KEY_F1:  return K_F1;   case KEY_F2:  return K_F2;
        case KEY_F3:  return K_F3;   case KEY_F4:  return K_F4;
        case KEY_F5:  return K_F5;   case KEY_F6:  return K_F6;
        case KEY_F7:  return K_F7;   case KEY_F8:  return K_F8;
        case KEY_F9:  return K_F9;   case KEY_F10: return K_F10;
        case KEY_F11: return K_F11;  case KEY_F12: return K_F12;
        case KEY_PAUSE: return K_PAUSE;
        // letters (Quake wants lowercase ascii)
        case KEY_A: return 'a'; case KEY_B: return 'b'; case KEY_C: return 'c';
        case KEY_D: return 'd'; case KEY_E: return 'e'; case KEY_F: return 'f';
        case KEY_G: return 'g'; case KEY_H: return 'h'; case KEY_I: return 'i';
        case KEY_J: return 'j'; case KEY_K: return 'k'; case KEY_L: return 'l';
        case KEY_M: return 'm'; case KEY_N: return 'n'; case KEY_O: return 'o';
        case KEY_P: return 'p'; case KEY_Q: return 'q'; case KEY_R: return 'r';
        case KEY_S: return 's'; case KEY_T: return 't'; case KEY_U: return 'u';
        case KEY_V: return 'v'; case KEY_W: return 'w'; case KEY_X: return 'x';
        case KEY_Y: return 'y'; case KEY_Z: return 'z';
        // number row
        case KEY_1: return '1'; case KEY_2: return '2'; case KEY_3: return '3';
        case KEY_4: return '4'; case KEY_5: return '5'; case KEY_6: return '6';
        case KEY_7: return '7'; case KEY_8: return '8'; case KEY_9: return '9';
        case KEY_0: return '0';
        case KEY_MINUS:      return '-';
        case KEY_EQUAL:      return '=';
        case KEY_LEFTBRACE:  return '[';
        case KEY_RIGHTBRACE: return ']';
        case KEY_SEMICOLON:  return ';';
        case KEY_APOSTROPHE: return '\'';
        case KEY_GRAVE:      return '`';   // console toggle
        case KEY_BACKSLASH:  return '\\';
        case KEY_COMMA:      return ',';
        case KEY_DOT:        return '.';
        case KEY_SLASH:      return '/';
        default: (void)shifted_out; return 0;
    }
}

static void kbd_poll(int fd)
{
    struct input_event iev[64];
    int n, k;
    for (;;) {
        n = read(fd, iev, sizeof(iev));
        if (n <= 0) break;
        for (k = 0; k < n / (int)sizeof(iev[0]); k++) {
            struct input_event *e = &iev[k];
            int qkey;
            if (e->type != EV_KEY) continue;
            if (e->value == 2) continue;            // ignore autorepeat
            qkey = kbd_translate(e->code, 0);
            if (qkey) Key_Event(qkey, e->value ? true : false);
        }
        if (n < (int)sizeof(iev)) break;
    }
}

// ==========================================================================
//  Gamepad: read raw, recompute derived state
// ==========================================================================
static float stick_norm(const stickaxis_t *s)
{
    float v;
    if (!s->have) return 0.0f;
    v = (float)(s->val - s->center) / (float)s->half;   // -1..1
    if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
    if (v > -0.20f && v < 0.20f) v = 0.0f;              // dead zone
    return v;
}

/* ---------------------------------------------------------------------------
 * DS4 control scheme (from CONTROLS.csv, verified truth table in CONTROLS.md).
 * The design: MOVE+STRAFE on the left stick / d-pad, LOOK on the right stick
 * AND the four face buttons -- so aiming works identically on pads with no
 * analog sticks (a digital "look pad"). Positions compose the DS4 layout
 * (gp_btn[], codes 0x130+) and the generic layout (gp_gbtn[], codes 0x120+, e.g.
 * the Logitech Precision: printed button N -> gp_gbtn[N-1], d-pad on ABS_X/Y).
 * Only one pad is ever connected, so OR-ing the two layouts is safe.
 * ------------------------------------------------------------------------- */
static void pad_recompute(void)
{
    int i, in_game = (key_dest == key_game);
    int face_left, face_down, face_right, face_up, b_jump, b_fire, b_run, b_next, b_menu;
    float lx = stick_norm(&ax_lx), ly = stick_norm(&ax_ly);
    float rx = stick_norm(&ax_rx), ry = stick_norm(&ax_ry);

    // Digital directions: hat + center-resting RX/RY (a Saturn-style d-pad) +
    // the left stick past halfway (used for movement fallback and menu arrows).
    dig_left  = hatx < 0;  dig_right = hatx > 0;
    dig_up    = haty < 0;  dig_down  = haty > 0;
    for (i = 0; i < 2; i++) {
        if (!rxry_on[i]) continue;
        if (rxry_val[i] < rxry_center[i] - rxry_dead[i]) { if (i) dig_up = 1; else dig_left = 1; }
        else if (rxry_val[i] > rxry_center[i] + rxry_dead[i]) { if (i) dig_down = 1; else dig_right = 1; }
    }
    if (lx < -0.5f) dig_left = 1; else if (lx > 0.5f) dig_right = 1;
    if (ly < -0.5f) dig_up = 1;   else if (ly > 0.5f) dig_down = 1;

    // MOVE + STRAFE: left stick (analog) with the d-pad as a digital fallback.
    mv_fwd = -ly;
    if (mv_fwd == 0.0f) { if (dig_up) mv_fwd = 1.0f; else if (dig_down) mv_fwd = -1.0f; }
    mv_side = lx;
    if (mv_side == 0.0f) { if (dig_right) mv_side = 1.0f; else if (dig_left) mv_side = -1.0f; }

    // Physical positions, composed from both layouts (see header note).
    // DS4 face indices 0-3 = Square/Cross/Circle/Triangle; the Logitech's first
    // four buttons (gp_gbtn[0-3]) are its Look Left/Down/Right/Up in the same
    // order, so one set of ORs covers both.
    face_left  = gp_btn[GP_SQUARE]   || gp_gbtn[0];   // Look Left
    face_down  = gp_btn[GP_CROSS]    || gp_gbtn[1];   // Look Down
    face_right = gp_btn[GP_CIRCLE]   || gp_gbtn[2];   // Look Right
    face_up    = gp_btn[GP_TRIANGLE] || gp_gbtn[3];   // Look Up
    b_jump = gp_btn[GP_L1] || gp_gbtn[4];             // L1 shoulder / Logitech btn5
    b_fire = gp_btn[GP_R1] || gp_gbtn[5];             // R1 shoulder / Logitech btn6
    // Run/Next-weapon: DS4 analog triggers at ~30% (trig_pulled) + stick clicks;
    // Logitech plain buttons 7/8. Digital trigger press kept as a fallback.
    b_run  = trig_pulled(0) || gp_btn[GP_L2] || gp_btn[GP_L3] || gp_btn[GP_R3] || gp_gbtn[6];
    b_next = trig_pulled(1) || gp_btn[GP_R2] || gp_gbtn[7];
    b_menu = gp_btn[GP_OPTIONS] || gp_gbtn[8] || gp_gbtn[9];

    // LOOK: right stick (analog) plus the face buttons (digital, full-rate turn/
    // pitch). Face-button look only applies in game; in a menu the face buttons
    // are Select/Back. Left/Right = turn, Up/Down = pitch (recenters on release,
    // handled in IN_Evdev_Move).
    look_yaw = rx;  look_pitch = ry;
    if (in_game) {
        if (face_left)  look_yaw   -= 1.0f;
        if (face_right) look_yaw   += 1.0f;
        if (face_up)    look_pitch -= 1.0f;
        if (face_down)  look_pitch += 1.0f;
    }
    if (look_yaw   >  1.0f) look_yaw   =  1.0f; if (look_yaw   < -1.0f) look_yaw   = -1.0f;
    if (look_pitch >  1.0f) look_pitch =  1.0f; if (look_pitch < -1.0f) look_pitch = -1.0f;

    // ---- Buttons -> Quake keys (rebuilt into want[] every frame) ----
    memset(qk_want, 0, sizeof(qk_want));

    if (in_game) {
        if (b_next && !prev_nextweap) in_impulse = 10;   // Next weapon (one-shot)
        prev_nextweap = b_next;
        if (b_fire) qk_mark(K_MOUSE1);                   // Fire
        if (b_jump) qk_mark(K_SPACE);                    // Jump
        if (b_run)  qk_mark(K_SHIFT);                    // Run (hold)
        if (b_menu) qk_mark(K_ESCAPE);                   // Menu
    } else {
        // Menu: d-pad / left stick navigate; buttons map to Select or Back by
        // physical position (see CONTROLS.csv). DS4 and Logitech both listed.
        if (dig_left)  qk_mark(K_LEFTARROW);
        if (dig_right) qk_mark(K_RIGHTARROW);
        if (dig_up)    qk_mark(K_UPARROW);
        if (dig_down)  qk_mark(K_DOWNARROW);

        if (gp_btn[GP_CROSS] || gp_btn[GP_CIRCLE] || gp_btn[GP_R1] ||
            gp_btn[GP_R2]    || gp_btn[GP_R3] ||
            gp_gbtn[1] || gp_gbtn[2] || gp_gbtn[5] || gp_gbtn[7] || gp_gbtn[9])
            qk_mark(K_ENTER);   // Select
        if (gp_btn[GP_SQUARE] || gp_btn[GP_TRIANGLE] || gp_btn[GP_L1] ||
            gp_btn[GP_L2]     || gp_btn[GP_OPTIONS]  || gp_btn[GP_L3] ||
            gp_gbtn[0] || gp_gbtn[3] || gp_gbtn[4] || gp_gbtn[6] || gp_gbtn[8])
            qk_mark(K_ESCAPE);  // Back
    }

    qk_reconcile();
}

static int pad_poll(int fd)     // returns 0 ok, -1 node died
{
    struct input_event iev[64];
    int n, k;
    for (;;) {
        n = read(fd, iev, sizeof(iev));
        if (n > 0) {
            for (k = 0; k < n / (int)sizeof(iev[0]); k++) {
                struct input_event *e = &iev[k];
                if (e->type == EV_KEY) {
                    int c = e->code;
                    if (c >= BTN_SOUTH && c < BTN_SOUTH + GP_NBTN)
                        gp_btn[c - BTN_SOUTH] = e->value ? 1 : 0;
                    else if (c >= BTN_JOYSTICK && c < BTN_JOYSTICK + GP_NGEN)
                        gp_gbtn[c - BTN_JOYSTICK] = e->value ? 1 : 0;
                } else if (e->type == EV_ABS) {
                    switch (e->code) {
                        case ABS_X:     ax_lx.val = e->value; break;
                        case ABS_Y:     ax_ly.val = e->value; break;
                        case ABS_Z:     ax_rx.val = e->value; break;
                        case ABS_RZ:    ax_ry.val = e->value; break;
                        case ABS_RX:    rxry_val[0] = e->value; break;
                        case ABS_RY:    rxry_val[1] = e->value; break;
                        case ABS_HAT0X: hatx = e->value; break;
                        case ABS_HAT0Y: haty = e->value; break;
                        default: break;
                    }
                }
            }
            if (n == (int)sizeof(iev)) continue;    // buffer was full, more?
            break;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;   // drained
        return -1;                                   // EOF / ENODEV: node died
    }
    return 0;
}

// ==========================================================================
//  Public API
// ==========================================================================
void IN_Evdev_Init (void)
{
    int i;
    for (i = 0; i < MAX_DEVS; i++) { devs[i].fd = -1; devs[i].kind = DEV_NONE; devs[i].idx = -1; }
    memset(qk_have, 0, sizeof(qk_have));
    last_scan = 0;
    scan_verbose = 3;
    (void)dev_is_open;
}

void IN_Evdev_Shutdown (void)
{
    int i, zero = 0;
    for (i = 0; i < MAX_DEVS; i++) {
        if (devs[i].fd >= 0) {
            ioctl(devs[i].fd, EVIOCGRAB, &zero);
            close(devs[i].fd);
            devs[i].fd = -1; devs[i].kind = DEV_NONE; devs[i].idx = -1;
        }
    }
    qk_release_all();
}

void IN_Evdev_Poll (void)
{
    Uint32 now = SDL_GetTicks();
    int i, have_pad = 0;

    if (now - last_scan >= 1000) {   // (re)scan for hotplugged devices ~1/s
        last_scan = now;
        scan_devices();
    }

    for (i = 0; i < MAX_DEVS; i++) {
        if (devs[i].fd < 0) continue;
        if (devs[i].kind == DEV_KBD) {
            kbd_poll(devs[i].fd);
            // (a keyboard read error is rare; leave it, scan re-grabs on EBUSY)
        } else if (devs[i].kind == DEV_PAD) {
            if (pad_poll(devs[i].fd) < 0) {
                int zero = 0;
                ioctl(devs[i].fd, EVIOCGRAB, &zero);
                close(devs[i].fd);
                devs[i].fd = -1; devs[i].kind = DEV_NONE; devs[i].idx = -1;
                Con_Printf("evdev: gamepad node closed -- will re-open\n");
                pad_reset_state();
                continue;
            }
            have_pad = 1;
        }
    }

    if (have_pad) pad_recompute();
    else { memset(qk_want, 0, sizeof(qk_want)); qk_reconcile(); }
}

void IN_Evdev_Move (usercmd_t *cmd)
{
    int i, have_pad = 0;
    for (i = 0; i < MAX_DEVS; i++)
        if (devs[i].fd >= 0 && devs[i].kind == DEV_PAD) { have_pad = 1; break; }
    if (!have_pad || key_dest != key_game) return;

    cmd->forwardmove += cl_forwardspeed.value * mv_fwd;
    cmd->sidemove    += cl_sidespeed.value    * mv_side;

    // Turn (yaw) at the keyboard turn rate; framerate-independent.
    cl.viewangles[YAW] -= cl_yawspeed.value * look_yaw * host_frametime;

    // Pitch with lookspring: while actively looking up/down, adjust and hold
    // (stop the auto-drift); on release, kick off Quake's pitch drift so the
    // view springs back to level -- V_DriftPitch() (called each frame in
    // V_CalcRefdef) does the smooth recenter.
    {
        static int prev_pitching = 0;
        int pitching = (look_pitch != 0.0f);
        if (pitching) {
            cl.viewangles[PITCH] += cl_pitchspeed.value * look_pitch * host_frametime;
            if (cl.viewangles[PITCH] > 80)  cl.viewangles[PITCH] = 80;
            if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
            V_StopPitchDrift();
        } else if (prev_pitching) {
            V_StartPitchDrift();
        }
        prev_pitching = pitching;
    }
}

#endif // __webos__
