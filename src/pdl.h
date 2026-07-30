/*
 * ===========================================================================
 *
 *       Filename:  pdl.h
 *
 *    Description:  Parts of PDL that I use
 *
 *        Version:  1.0
 *        Created:  01/26/2010 01:32:23 PM
 *
 *         Author:  Will Dietz (WD), w@wdtz.org
 *        Company:  dtzTech
 *
 * ===========================================================================
 */

#define PDL_ORIENTATION_BOTTOM 0
#define PDL_ORIENTATION_RIGHT 1
#define PDL_ORIENTATION_TOP 2
#define PDL_ORIENTATION_LEFT 3

#define PDL_TRUE 1
#define PDL_FALSE 0

#define PDL_NOERROR 0

// Orientation: 0=bottom, 1= right, 2=top, 3=left
// Note: this controls the notification popup location, it does not flip location 0,0
int PDL_SetOrientation(int Orientation);

// MUST be called before SDL_Init for GPU/GL access -- the PDK integration layer
// sets up the app's compositor surface here. The software build got away with
// never calling it; an OpenGL ES context does not.
int PDL_Init(int flags);

//Opens web browser and points it to url
int PDL_LaunchBrowser(char * url);

//Cleans things up, haven't fully traced everything it's doing yet
int PDL_Quit();

//Enables/Disables Notifications
void PDL_BannerMessagesEnable(int enable);

//Enables/Disables Custom Pause UI
void PDL_CustomPauseUiEnable(int enable);
