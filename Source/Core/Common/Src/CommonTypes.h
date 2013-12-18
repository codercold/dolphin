// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.


// This header contains type definitions that are shared between the Dolphin core and
// other parts of the code. Any definitions that are only used by the core should be
// placed in "Common.h" instead.

#ifndef _COMMONTYPES_H_
#define _COMMONTYPES_H_

#include <cinttypes>
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
#include <tchar.h>
#endif

#ifndef GEKKO
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

#endif // _COMMONTYPES_H_
