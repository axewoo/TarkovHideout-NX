#pragma once

// The app is written against libnx. On the Switch that is the real thing; the desktop build
// gets SDL-backed stand-ins for the same calls plus a few pointer helpers (desktop/nxshim.h).
#ifdef __SWITCH__
#include <switch.h>

#include <string>

#define ROMFS_ROOT "romfs:/"
#define NX_WINDOW_FLAGS 0

// Desktop-only input; the Switch has none of these.
inline float nxWheel() { return 0; }
inline int nxTabKey() { return -1; }
inline bool nxPointer(int &, int &) { return false; }
inline bool nxTouchCancelled() { return false; }
inline const std::string &nxButtonLabel(const std::string &b) { return b; }
inline float nxUiScale() { return 1; }
inline void nxLayoutSize(int &w, int &h) {
    w = 1280;
    h = 720;
}
inline int nxZoom() { return 100; }
inline void nxSetZoom(int) {}
inline void nxWindowCreated(struct SDL_Window *, struct SDL_Renderer *) {}
inline bool nxDesktop() { return false; }
#else
#include "nxshim.h"
#endif
