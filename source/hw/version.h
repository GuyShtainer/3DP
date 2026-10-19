/* version.h -- the app version. The Makefile reads the three numbers below (grep, for makerom -major/-minor/-micro); only the hardware layer and the
 * boot code include this header, never a game object, so a version bump does not rebuild the game (the exit (a) hash is unaffected by it). */
#ifndef APP_VERSION_H
#define APP_VERSION_H

#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 1
#define APP_VERSION_MICRO 0
#define APP_VERSION_STR "0.1.0"

#endif
