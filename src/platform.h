#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef WINVER
#define WINVER 0x0601 // win 7 or later
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <assert.h>
#include <objbase.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <windows.h>

