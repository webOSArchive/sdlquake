// evread.c -- dump raw evdev events with human-readable names, to build the
// DS4 truth table. Usage: evread /dev/input/event3
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>

static const char *keyname(int c) {
    switch (c) {
        case 0x120: return "BTN_TRIGGER(0x120)";
        case 0x121: return "BTN_THUMB(0x121)";
        case 0x122: return "BTN_THUMB2(0x122)";
        case 0x123: return "BTN_TOP(0x123)";
        case 0x124: return "BTN_TOP2(0x124)";
        case 0x125: return "BTN_PINKIE(0x125)";
        case 0x126: return "BTN_BASE(0x126)";
        case 0x127: return "BTN_BASE2(0x127)";
        case 0x130: return "BTN_SOUTH/A(0x130)";
        case 0x131: return "BTN_EAST/B(0x131)";
        case 0x132: return "BTN_C(0x132)";
        case 0x133: return "BTN_NORTH/X(0x133)";
        case 0x134: return "BTN_WEST/Y(0x134)";
        case 0x135: return "BTN_Z(0x135)";
        case 0x136: return "BTN_TL/L1(0x136)";
        case 0x137: return "BTN_TR/R1(0x137)";
        case 0x138: return "BTN_TL2/L2(0x138)";
        case 0x139: return "BTN_TR2/R2(0x139)";
        case 0x13a: return "BTN_SELECT(0x13a)";
        case 0x13b: return "BTN_START(0x13b)";
        case 0x13c: return "BTN_MODE/PS(0x13c)";
        case 0x13d: return "BTN_THUMBL/L3(0x13d)";
        case 0x13e: return "BTN_THUMBR/R3(0x13e)";
        case 0x220: return "BTN_DPAD_UP(0x220)";
        case 0x221: return "BTN_DPAD_DOWN(0x221)";
        case 0x222: return "BTN_DPAD_LEFT(0x222)";
        case 0x223: return "BTN_DPAD_RIGHT(0x223)";
        default: return "BTN_?";
    }
}
static const char *absname(int c) {
    switch (c) {
        case 0x00: return "ABS_X";   case 0x01: return "ABS_Y";
        case 0x02: return "ABS_Z";   case 0x03: return "ABS_RX";
        case 0x04: return "ABS_RY";  case 0x05: return "ABS_RZ";
        case 0x10: return "ABS_HAT0X"; case 0x11: return "ABS_HAT0Y";
        default: return "ABS_?";
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/dev/input/event3";
    int fd = open(path, O_RDONLY);
    struct input_event e;
    if (fd < 0) { printf("open %s failed\n", path); return 1; }
    { int one = 1; if (ioctl(fd, EVIOCGRAB, &one) != 0) printf("(grab failed, reading shared)\n"); }
    printf("reading %s (grabbed; press one control at a time)\n", path);
    fflush(stdout);
    while (read(fd, &e, sizeof e) == (int)sizeof e) {
        if (e.type == EV_KEY) {
            printf("KEY  %-22s val=%d\n", keyname(e.code), e.value);
            fflush(stdout);
        } else if (e.type == EV_ABS) {
            // Suppress resting jitter: only report a stick/trigger axis when it
            // is pushed well off centre; always report the hat.
            int hat = (e.code == 0x10 || e.code == 0x11);
            if (hat || e.value < 100 || e.value > 156) {
                printf("ABS  %-10s(0x%02x) val=%d\n", absname(e.code), e.code, e.value);
                fflush(stdout);
            }
        }
    }
    return 0;
}
