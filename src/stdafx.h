// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

// C RunTime Header Files
#include <stdlib.h>
#if defined(_WIN32) || defined(OS_WIN)
#include <malloc.h>
#endif
#include <memory.h>

#ifndef WITH_SDL
#define WITH_SDL
#endif

#if !defined(_WIN32)
#include <strings.h>
#define stricmp strcasecmp
#define strnicmp strncasecmp
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#endif

#define VERSION_STR "0.2.1"

#pragma warning(disable: 4530)
// TODO: reference additional headers your program requires here
