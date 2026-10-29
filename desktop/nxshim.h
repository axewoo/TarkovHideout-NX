#pragma once

// Desktop stand-ins for the libnx calls the app uses, backed by SDL: keyboard, mouse and any
// game controller become the Switch pad and touch screen, swkbd becomes an in-window text box,
// romfs is the data folder next to the executable and the working directory is the user's
// data folder (so APP_DIR "." keeps the Switch code's relative paths).

#include <SDL2/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#include <direct.h>
inline int mkdir(const char *path, int) { return _mkdir(path); }
#endif

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint32_t Result;

#define R_FAILED(rc) ((rc) != 0)
#define R_SUCCEEDED(rc) ((rc) == 0)

#define APP_DIR "."
#define ROMFS_ROOT nxRomfsRoot()
#define NX_WINDOW_FLAGS (SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI)

constexpr u64 HidNpadButton_A = 1ULL << 0;
constexpr u64 HidNpadButton_B = 1ULL << 1;
constexpr u64 HidNpadButton_X = 1ULL << 2;
constexpr u64 HidNpadButton_Y = 1ULL << 3;
constexpr u64 HidNpadButton_StickL = 1ULL << 4;
constexpr u64 HidNpadButton_StickR = 1ULL << 5;
constexpr u64 HidNpadButton_L = 1ULL << 6;
constexpr u64 HidNpadButton_R = 1ULL << 7;
constexpr u64 HidNpadButton_ZL = 1ULL << 8;
constexpr u64 HidNpadButton_ZR = 1ULL << 9;
constexpr u64 HidNpadButton_Plus = 1ULL << 10;
constexpr u64 HidNpadButton_Minus = 1ULL << 11;
constexpr u64 HidNpadButton_Left = 1ULL << 12;
constexpr u64 HidNpadButton_Up = 1ULL << 13;
constexpr u64 HidNpadButton_Right = 1ULL << 14;
constexpr u64 HidNpadButton_Down = 1ULL << 15;
constexpr u64 HidNpadButton_StickLLeft = 1ULL << 16;
constexpr u64 HidNpadButton_StickLUp = 1ULL << 17;
constexpr u64 HidNpadButton_StickLRight = 1ULL << 18;
constexpr u64 HidNpadButton_StickLDown = 1ULL << 19;
constexpr u64 HidNpadButton_StickRLeft = 1ULL << 20;
constexpr u64 HidNpadButton_StickRUp = 1ULL << 21;
constexpr u64 HidNpadButton_StickRRight = 1ULL << 22;
constexpr u64 HidNpadButton_StickRDown = 1ULL << 23;
constexpr u64 HidNpadButton_AnyLeft = HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_StickRLeft;
constexpr u64 HidNpadButton_AnyUp = HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp;
constexpr u64 HidNpadButton_AnyRight = HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_StickRRight;
constexpr u64 HidNpadButton_AnyDown = HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown;
constexpr u32 HidNpadStyleSet_NpadStandard = 0;

struct HidAnalogStickState {
    s32 x, y;
};

struct PadState {
    u64 buttons = 0, old = 0;
    HidAnalogStickState sticks[2] = {};
};

void padConfigureInput(u32 maxPlayers, u32 styles);
void padInitializeDefault(PadState *pad);
void padUpdate(PadState *pad);
inline u64 padGetButtons(const PadState *pad) { return pad->buttons; }
inline u64 padGetButtonsDown(const PadState *pad) { return pad->buttons & ~pad->old; }
inline HidAnalogStickState padGetStickPos(const PadState *pad, int i) { return pad->sticks[i & 1]; }

struct HidTouchState {
    u32 x, y;
};
struct HidTouchScreenState {
    s32 count;
    HidTouchState touches[1];
};
inline void hidInitializeTouchScreen() {}
size_t hidGetTouchScreenStates(HidTouchScreenState *states, size_t count);

u64 armGetSystemTick();
u64 armGetSystemTickFreq();
u64 armTicksToNs(u64 ticks);

bool appletMainLoop();
Result romfsInit();
inline Result romfsExit() { return 0; }
inline Result socketInitializeDefault() { return 0; }
inline void socketExit() {}

enum PlServiceType { PlServiceType_User };
enum PlSharedFontType { PlSharedFontType_Standard };
struct PlFontData {
    void *address;
    u32 size;
};
inline Result plInitialize(PlServiceType) { return 0; }
inline void plExit() {}
Result plGetSharedFontByType(PlFontData *font, PlSharedFontType type);

enum SwkbdType { SwkbdType_Normal, SwkbdType_NumPad };
struct SwkbdConfig {
    std::string header, guide, initial;
    size_t maxLen = 128;
    bool numeric = false;
};
inline Result swkbdCreate(SwkbdConfig *, s32) { return 0; }
inline void swkbdConfigMakePresetDefault(SwkbdConfig *) {}
inline void swkbdConfigSetType(SwkbdConfig *c, SwkbdType t) { c->numeric = t == SwkbdType_NumPad; }
inline void swkbdConfigSetHeaderText(SwkbdConfig *c, const char *s) { c->header = s; }
inline void swkbdConfigSetGuideText(SwkbdConfig *c, const char *s) { c->guide = s; }
inline void swkbdConfigSetInitialText(SwkbdConfig *c, const char *s) { c->initial = s; }
inline void swkbdConfigSetStringLenMax(SwkbdConfig *c, u32 n) { c->maxLen = n; }
// Runs a modal text box in the app window; fails when cancelled, like the Switch keyboard.
Result swkbdShow(SwkbdConfig *c, char *out, size_t outSize);
inline void swkbdClose(SwkbdConfig *) {}

// Desktop extras.
const std::string &nxRomfsRoot();
float nxWheel();                // wheel notches since the last pad update, up is positive
int nxTabKey();                 // tab picked with a number key this update, -1 if none
bool nxPointer(int &x, int &y); // mouse position in the 1280 x 720 layout
bool nxTouchCancelled();        // the touch that just ended turned into a pinch, so it is no tap
// Keyboard name for a Switch button label when the keyboard was used last.
const std::string &nxButtonLabel(const std::string &button);
float nxUiScale();              // window pixels per layout pixel
void nxWindowCreated(SDL_Window *window, SDL_Renderer *renderer);
inline bool nxDesktop() { return true; }
