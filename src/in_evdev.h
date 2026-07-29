// in_evdev.h -- webOS direct-evdev input for USB/Bluetooth game controllers
// and physical (USB/BT) keyboards. See in_evdev.c for the why.
#ifndef IN_EVDEV_H
#define IN_EVDEV_H

#include "quakedef.h"

#ifdef __webos__

// Open/close the evdev subsystem (safe to call if no devices present).
void IN_Evdev_Init (void);
void IN_Evdev_Shutdown (void);

// Drain all controller/keyboard nodes once per frame and inject Quake
// Key_Event()s. Call from Sys_SendKeyEvents(), after SDL is pumped.
void IN_Evdev_Poll (void);

// Apply the controller's analog sticks to a usercmd (movement) and to the
// view angles (look). Call from IN_Move().
void IN_Evdev_Move (usercmd_t *cmd);

#endif // __webos__

#endif // IN_EVDEV_H
