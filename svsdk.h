#ifndef SVSDK_H
#define SVSDK_H

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <windows.h>
#endif

extern "C" {
#include "sdk/include/SVNETSDK.h"
}

// 移除可能的 min/max 宏污染
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

#endif // SVSDK_H

