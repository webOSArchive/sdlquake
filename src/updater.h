// updater.h -- App Museum II update check.
//
// Asks the catalog whether a newer build of this app exists and, if so, says so
// once in the Quake console (which also shows briefly on screen). Nothing more:
// no dialogs, no downloading, no install prompt.
//
// This is a reference implementation for modern webOS retrofits, so the shape
// matters as much as the behaviour:
//   * the network call runs on a BACKGROUND THREAD and never blocks a frame;
//   * the worker touches no engine state -- it only fills buffers and sets a
//     flag, and the MAIN thread does all the Con_Printf'ing, because Quake's
//     console is not thread-safe;
//   * every failure is silent. No catalog entry, no network, no DNS, a
//     malformed reply: the player simply never hears about it.
#ifndef UPDATER_H
#define UPDATER_H

// Name this app is listed under in the App Museum catalog. The lookup is
// case-insensitive; spaces are fine and get URL-encoded.
#define UPDATER_APP_NAME "Quake HD"

// The build script parses the version out of build/webos/hd/appinfo.json and
// passes it as -DQUAKEHD_VERSION_RAW=1.2.3 (unquoted, because CFLAGS is
// expanded unquoted and a quoted string would not survive word-splitting), so
// the two can never drift apart. Stringify it here.
#ifdef QUAKEHD_VERSION_RAW
#define UPD_STR2(x) #x
#define UPD_STR(x)  UPD_STR2(x)
#define QUAKEHD_VERSION UPD_STR(QUAKEHD_VERSION_RAW)
#else
#define QUAKEHD_VERSION "0.0.0"
#endif

// The update check ships only with the TouchPad HD build. The long-stable
// software/phone build does not compile updater.c, so it gets no-ops -- host.c
// can then call these unconditionally without the two builds diverging.
// (Without this, the software build fails to LINK: host.c references
// Updater_Init/Poll/Shutdown that nothing defines. That is exactly how it broke
// when the update check landed, and it went unnoticed because only the HD
// target is ever built here.)
#ifdef GLQUAKE

// Kick off the check. Safe to call more than once; only the first does work.
void Updater_Init (void);

// Call once per frame from the main thread. Prints the notice when (and only
// when) the background check has come back positive.
void Updater_Poll (void);

// Reap the worker at shutdown.
void Updater_Shutdown (void);

#else

#define Updater_Init()      ((void)0)
#define Updater_Poll()      ((void)0)
#define Updater_Shutdown()  ((void)0)

#endif

#endif // UPDATER_H
