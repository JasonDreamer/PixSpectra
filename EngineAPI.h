#pragma once
#include "Protocol.h"
// Engine runs inside After Effects. No executable, IPC or audio device is used.
using PixConfigure = int (__cdecl*)(const uint8_t*, uint32_t, uint8_t**, uint32_t*, char*, uint32_t,void*);
using PixRender = int (__cdecl*)(const pix::Request*, const uint8_t*, const float*, float*, char*, uint32_t);
using PixFree = void (__cdecl*)(void*);
using PixShutdown = void (__cdecl*)();
