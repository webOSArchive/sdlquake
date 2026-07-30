// evread.c -- dump an evdev device's capabilities and then stream its events
// with human-readable names, to build a controller truth table.
//
//   evread [-n] [/dev/input/eventN]     -n = do NOT EVIOCGRAB (read shared)
//
// Prints, at startup: device name, every EV_KEY code it advertises, and every
// EV_ABS axis with min/max/flat/resting value -- that resting value is what
// tells a d-pad axis (rests centred) from an analog trigger (rests at an end).
// Then one line per event, suppressing resting jitter per-axis using the axis's
// OWN range (not a hardcoded 0-255 assumption).
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#ifndef BTN_SOUTH
#define BTN_SOUTH 0x130
#endif

static const char *keyname(int c) {
    switch (c) {
        case 0x120: return "BTN_TRIGGER";  case 0x121: return "BTN_THUMB";
        case 0x122: return "BTN_THUMB2";   case 0x123: return "BTN_TOP";
        case 0x124: return "BTN_TOP2";     case 0x125: return "BTN_PINKIE";
        case 0x126: return "BTN_BASE";     case 0x127: return "BTN_BASE2";
        case 0x128: return "BTN_BASE3";    case 0x129: return "BTN_BASE4";
        case 0x12a: return "BTN_BASE5";    case 0x12b: return "BTN_BASE6";
        case 0x12f: return "BTN_DEAD";
        case 0x130: return "BTN_SOUTH/A";  case 0x131: return "BTN_EAST/B";
        case 0x132: return "BTN_C";        case 0x133: return "BTN_NORTH/X";
        case 0x134: return "BTN_WEST/Y";   case 0x135: return "BTN_Z";
        case 0x136: return "BTN_TL";       case 0x137: return "BTN_TR";
        case 0x138: return "BTN_TL2";      case 0x139: return "BTN_TR2";
        case 0x13a: return "BTN_SELECT";   case 0x13b: return "BTN_START";
        case 0x13c: return "BTN_MODE";     case 0x13d: return "BTN_THUMBL";
        case 0x13e: return "BTN_THUMBR";
        case 0x220: return "BTN_DPAD_UP";  case 0x221: return "BTN_DPAD_DOWN";
        case 0x222: return "BTN_DPAD_LEFT";case 0x223: return "BTN_DPAD_RIGHT";
        default: return "BTN_?";
    }
}

static const char *absname(int c) {
    switch (c) {
        case 0x00: return "ABS_X";        case 0x01: return "ABS_Y";
        case 0x02: return "ABS_Z";        case 0x03: return "ABS_RX";
        case 0x04: return "ABS_RY";       case 0x05: return "ABS_RZ";
        case 0x06: return "ABS_THROTTLE"; case 0x07: return "ABS_RUDDER";
        case 0x08: return "ABS_WHEEL";    case 0x09: return "ABS_GAS";
        case 0x0a: return "ABS_BRAKE";
        case 0x10: return "ABS_HAT0X";    case 0x11: return "ABS_HAT0Y";
        case 0x12: return "ABS_HAT1X";    case 0x13: return "ABS_HAT1Y";
        case 0x14: return "ABS_HAT2X";    case 0x15: return "ABS_HAT2Y";
        case 0x16: return "ABS_HAT3X";    case 0x17: return "ABS_HAT3Y";
        default: return "ABS_?";
    }
}

#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)

static int test_bit(const unsigned long *b, int i) {
    return (b[i / (8 * sizeof(long))] >> (i % (8 * sizeof(long)))) & 1UL;
}

/* Per-axis gate: report only once the axis moves past 25% of its range away
 * from where it was resting when we opened it, then again when it comes back. */
static int   ax_rest[ABS_MAX + 1];
static int   ax_gate[ABS_MAX + 1];
static int   ax_hot[ABS_MAX + 1];
static char  ax_known[ABS_MAX + 1];

int main(int argc, char **argv)
{
    const char *path = NULL;
    int fd, i, grab = 1;
    char name[128];
    unsigned long keyb[NBITS(KEY_MAX)], absb[NBITS(ABS_MAX)];
    struct input_event e;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) grab = 0;
        else path = argv[i];
    }
    if (!path) path = "/dev/input/event3";

    fd = open(path, O_RDONLY);
    if (fd < 0) { printf("open %s failed\n", path); return 1; }

    name[0] = 0;
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    printf("== %s : \"%s\"\n", path, name);

    memset(keyb, 0, sizeof(keyb));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0) {
        printf("-- EV_KEY codes advertised:\n");
        for (i = 0; i <= KEY_MAX; i++)
            if (test_bit(keyb, i)) {
                if (i >= BTN_SOUTH && i < BTN_SOUTH + 16)
                    printf("   0x%03x %-14s  (gp_btn[%d])\n", i, keyname(i), i - BTN_SOUTH);
                else
                    printf("   0x%03x %s\n", i, keyname(i));
            }
    }

    memset(absb, 0, sizeof(absb));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absb)), absb) >= 0) {
        printf("-- EV_ABS axes (rest = value at open):\n");
        for (i = 0; i <= ABS_MAX; i++) {
            struct input_absinfo ai;
            int centre, quarter;
            if (!test_bit(absb, i)) continue;
            if (ioctl(fd, EVIOCGABS(i), &ai) != 0) continue;
            centre  = (ai.minimum + ai.maximum) / 2;
            quarter = (ai.maximum - ai.minimum) / 4;
            printf("   0x%02x %-12s min=%d max=%d flat=%d rest=%d  -> %s\n",
                   i, absname(i), ai.minimum, ai.maximum, ai.flat, ai.value,
                   (quarter > 0 && ai.value >= centre - quarter &&
                    ai.value <= centre + quarter) ? "CENTRE-resting (stick/dpad)"
                                                  : "END-resting (trigger?)");
            ax_known[i] = 1;
            ax_rest[i]  = ai.value;
            /* Report once the axis leaves a quarter of its range -- but only a
             * real analog axis (0-255 and friends) has range to spare. A hat
             * spans -1..1, and an 8-way direction-code axis spans 0..7, where a
             * quarter-range gate would silently swallow the low direction
             * values. Anything narrow reports on ANY change from rest. */
            ax_gate[i]  = (ai.maximum - ai.minimum) > 16 ? quarter : 0;
        }
    }

    if (grab) {
        int one = 1;
        if (ioctl(fd, EVIOCGRAB, &one) != 0)
            /* EBUSY here means ANOTHER process holds an exclusive grab -- and
             * then a shared reader receives NOTHING, so the capture would be
             * silently empty. Any other errno and shared reading still works. */
            printf("(grab failed: %s -- shared reads %s)\n", strerror(errno),
                   errno == EBUSY ? "WILL GET NOTHING" : "still work");
        else
            printf("(grabbed exclusively)\n");
    } else {
        printf("(not grabbing, reading shared)\n");
    }

    printf("-- streaming events; press ONE control at a time --\n");
    fflush(stdout);

    while (read(fd, &e, sizeof e) == (int)sizeof e) {
        if (e.type == EV_KEY) {
            if (e.value == 2) continue;                 /* autorepeat */
            if (e.code >= BTN_SOUTH && e.code < BTN_SOUTH + 16)
                printf("KEY  0x%03x %-14s gp_btn[%-2d] %s\n", e.code,
                       keyname(e.code), e.code - BTN_SOUTH,
                       e.value ? "DOWN" : "up");
            else
                printf("KEY  0x%03x %-14s            %s\n", e.code,
                       keyname(e.code), e.value ? "DOWN" : "up");
            fflush(stdout);
        } else if (e.type == EV_ABS) {
            int c = e.code, hot;
            if (c < 0 || c > ABS_MAX || !ax_known[c]) continue;
            hot = (e.value > ax_rest[c] + ax_gate[c] ||
                   e.value < ax_rest[c] - ax_gate[c]);
            if (hot || ax_hot[c]) {                     /* edges + while moved */
                printf("ABS  0x%02x %-12s val=%-6d (rest=%d) %s\n", c,
                       absname(c), e.value, ax_rest[c], hot ? "MOVED" : "back");
                fflush(stdout);
            }
            ax_hot[c] = hot;
        }
    }
    return 0;
}
