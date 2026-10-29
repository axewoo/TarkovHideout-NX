#include "nxshim.h"

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#else
#include <unistd.h>
#endif

#include "ui.h"

namespace {

SDL_Window *gWindow = nullptr;
SDL_Renderer *gRenderer = nullptr;
std::string gBase;      // folder of the executable, with a trailing slash
std::string gRomfs;     // bundled data folder
bool gQuit = false;
float gWheel = 0;
int gTabKey = -1;
u64 gInjected = 0;      // one-frame presses from mouse buttons and shortcuts
bool gMouseDown = false;
bool gDownThisPump = false;  // a click shorter than a frame still shows as one frame of touch
bool gReleasePending = false;
int gMouseX = 0, gMouseY = 0;
bool gMouseIn = false;
// What the player used last decides how the button hints are labelled.
enum InputMode { MODE_KEYS, MODE_PAD, MODE_TOUCH };
#ifdef __ANDROID__
InputMode gMode = MODE_TOUCH;
#else
InputMode gMode = MODE_KEYS;
#endif
bool gPinching = false;        // two fingers on the screen: zoom, no dragging or tapping
bool gTouchCancelled = false;
std::vector<SDL_GameController *> gPads;
std::vector<char> gFont;
bool gSwallow = false;  // keys still held from the text box are not new presses
bool gWindowDirty = false;
Uint32 gWindowSaved = 0;

const char *kWindowFile = "window.cfg";

void saveWindow() {
#ifdef __ANDROID__
    return;
#endif
    if (!gWindow)
        return;
    Uint32 flags = SDL_GetWindowFlags(gWindow);
    if (flags & SDL_WINDOW_FULLSCREEN_DESKTOP)
        return;
    int x, y, w, h;
    SDL_GetWindowPosition(gWindow, &x, &y);
    SDL_GetWindowSize(gWindow, &w, &h);
    bool max = flags & SDL_WINDOW_MAXIMIZED;
    // A maximized window keeps the size it had before, so restoring it un-maximizes sensibly.
    std::ifstream in(kWindowFile);
    int ox, oy, ow, oh, om;
    if (max && in >> ox >> oy >> ow >> oh >> om) {
        w = ow;
        h = oh;
    }
    in.close();
    std::ofstream out(kWindowFile, std::ios::trunc);
    out << x << ' ' << y << ' ' << w << ' ' << h << ' ' << (max ? 1 : 0) << '\n';
}

bool ctrl() {
    return SDL_GetModState() & KMOD_CTRL;
}

void handleKey(const SDL_KeyboardEvent &k) {
    SDL_Keycode key = k.keysym.sym;
    if (key == SDLK_AC_BACK) {
        gInjected |= HidNpadButton_B;
        return;
    }
    gMode = MODE_KEYS;
    if (key == SDLK_F11 || (key == SDLK_RETURN && (k.keysym.mod & KMOD_ALT))) {
        bool full = SDL_GetWindowFlags(gWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(gWindow, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        return;
    }
    if (k.repeat)
        return;
    if (ctrl() && key == SDLK_q)
        gInjected |= HidNpadButton_Plus;
    else if (ctrl() && key == SDLK_f)
        gInjected |= HidNpadButton_Y;
    else if (key == SDLK_TAB)
        gInjected |= (SDL_GetModState() & KMOD_SHIFT) ? HidNpadButton_L : HidNpadButton_R;
    else if (!ctrl() && key >= SDLK_1 && key <= SDLK_9)
        gTabKey = key - SDLK_1;
}

void pump() {
    if (gReleasePending) {
        gMouseDown = false;
        gReleasePending = false;
    }
    gDownThisPump = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: gQuit = true; break;
        case SDL_KEYDOWN: handleKey(e.key); break;
        case SDL_MOUSEWHEEL:
            gMode = MODE_KEYS;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            gWheel += e.wheel.preciseY * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
#else
            gWheel += e.wheel.y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
#endif
            break;
        case SDL_MOUSEBUTTONDOWN:
            gMode = e.button.which == SDL_TOUCH_MOUSEID ? MODE_TOUCH : MODE_KEYS;
            if (gPinching && e.button.which == SDL_TOUCH_MOUSEID)
                break;
            gMouseX = e.button.x;
            gMouseY = e.button.y;
            if (e.button.button == SDL_BUTTON_LEFT)
                gMouseDown = gDownThisPump = true;
            else if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_X1)
                gInjected |= HidNpadButton_B;
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (gDownThisPump)
                    gReleasePending = true;
                else
                    gMouseDown = false;
            }
            break;
        case SDL_MOUSEMOTION:
            if (gPinching && e.motion.which == SDL_TOUCH_MOUSEID)
                break;
            gMouseX = e.motion.x;
            gMouseY = e.motion.y;
            gMouseIn = true;
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_LEAVE)
                gMouseIn = false;
            else if (e.window.event == SDL_WINDOWEVENT_ENTER)
                gMouseIn = true;
            else if (e.window.event == SDL_WINDOWEVENT_MOVED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                     e.window.event == SDL_WINDOWEVENT_MAXIMIZED || e.window.event == SDL_WINDOWEVENT_RESTORED)
                gWindowDirty = true;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (SDL_GameController *c = SDL_GameControllerOpen(e.cdevice.which))
                gPads.push_back(c);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (size_t i = 0; i < gPads.size(); i++)
                if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gPads[i])) == e.cdevice.which) {
                    SDL_GameControllerClose(gPads[i]);
                    gPads.erase(gPads.begin() + i);
                    break;
                }
            break;
        case SDL_CONTROLLERBUTTONDOWN: gMode = MODE_PAD; break;
        case SDL_FINGERDOWN:
            // A new gesture starts with its first finger.
            if (SDL_GetNumTouchFingers(e.tfinger.touchId) <= 1)
                gPinching = false;
            break;
        case SDL_MULTIGESTURE:
            // Pinch: zoom around the fingers instead of dragging with the first one.
            if (e.mgesture.numFingers >= 2) {
                if (!gPinching && gMouseDown)
                    gTouchCancelled = true;
                gPinching = true;
                gMouseDown = false;
                gWheel += e.mgesture.dDist * 14;
                gMouseX = int(e.mgesture.x * 1280);
                gMouseY = int(e.mgesture.y * 720);
                gMouseIn = true;
            }
            break;
        default: break;
        }
    }
    if (gWindowDirty && SDL_GetTicks() - gWindowSaved > 1000) {
        saveWindow();
        gWindowDirty = false;
        gWindowSaved = SDL_GetTicks();
    }
}

bool keyDown(const Uint8 *ks, SDL_Keycode key) {
    return ks[SDL_GetScancodeFromKey(key)];
}

} // namespace

void padConfigureInput(u32, u32) {}

void padInitializeDefault(PadState *pad) {
    *pad = PadState();
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void padUpdate(PadState *pad) {
    pad->old = pad->buttons;
    gWheel = 0;
    gTabKey = -1;
    pump();

    u64 b = gInjected;
    gInjected = 0;
    // Keys follow the layout (Q is Q on AZERTY too), so the on-screen names stay right.
    if (SDL_GetKeyboardFocus() == gWindow && !ctrl()) {
        const Uint8 *ks = SDL_GetKeyboardState(nullptr);
        if (ks[SDL_SCANCODE_UP]) b |= HidNpadButton_Up;
        if (ks[SDL_SCANCODE_DOWN]) b |= HidNpadButton_Down;
        if (ks[SDL_SCANCODE_LEFT]) b |= HidNpadButton_Left;
        if (ks[SDL_SCANCODE_RIGHT]) b |= HidNpadButton_Right;
        if (ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_KP_ENTER] || ks[SDL_SCANCODE_SPACE]) b |= HidNpadButton_A;
        if (ks[SDL_SCANCODE_ESCAPE] || ks[SDL_SCANCODE_BACKSPACE]) b |= HidNpadButton_B;
        if (keyDown(ks, SDLK_x)) b |= HidNpadButton_X;
        if (keyDown(ks, SDLK_y)) b |= HidNpadButton_Y;
        if (keyDown(ks, SDLK_q)) b |= HidNpadButton_L;
        if (keyDown(ks, SDLK_e)) b |= HidNpadButton_R;
        if (ks[SDL_SCANCODE_PAGEUP]) b |= HidNpadButton_ZL;
        if (ks[SDL_SCANCODE_PAGEDOWN]) b |= HidNpadButton_ZR;
        if (keyDown(ks, SDLK_f) || keyDown(ks, SDLK_MINUS) || ks[SDL_SCANCODE_KP_MINUS]) b |= HidNpadButton_Minus;
        if (keyDown(ks, SDLK_r)) b |= HidNpadButton_StickR;
    }

    HidAnalogStickState sticks[2] = {};
    for (SDL_GameController *c : gPads) {
        struct {
            SDL_GameControllerButton from;
            u64 to;
        } static const kMap[] = {
            {SDL_CONTROLLER_BUTTON_A, HidNpadButton_A},
            {SDL_CONTROLLER_BUTTON_B, HidNpadButton_B},
            {SDL_CONTROLLER_BUTTON_X, HidNpadButton_X},
            {SDL_CONTROLLER_BUTTON_Y, HidNpadButton_Y},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, HidNpadButton_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, HidNpadButton_R},
            {SDL_CONTROLLER_BUTTON_BACK, HidNpadButton_Minus},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, HidNpadButton_StickL},
            {SDL_CONTROLLER_BUTTON_RIGHTSTICK, HidNpadButton_StickR},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, HidNpadButton_Up},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, HidNpadButton_Down},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, HidNpadButton_Left},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, HidNpadButton_Right},
        };
        for (const auto &m : kMap)
            if (SDL_GameControllerGetButton(c, m.from))
                b |= m.to;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
            b |= HidNpadButton_ZL;
        if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
            b |= HidNpadButton_ZR;
        // libnx sticks point up for positive y; SDL's point down.
        int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) - 1;
        int rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        int ry = -SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY) - 1;
        if (std::abs(lx) > std::abs(sticks[0].x)) sticks[0].x = lx;
        if (std::abs(ly) > std::abs(sticks[0].y)) sticks[0].y = ly;
        if (std::abs(rx) > std::abs(sticks[1].x)) sticks[1].x = rx;
        if (std::abs(ry) > std::abs(sticks[1].y)) sticks[1].y = ry;
    }
    const int t = 16384;
    if (sticks[0].x < -t) b |= HidNpadButton_StickLLeft;
    if (sticks[0].x > t) b |= HidNpadButton_StickLRight;
    if (sticks[0].y > t) b |= HidNpadButton_StickLUp;
    if (sticks[0].y < -t) b |= HidNpadButton_StickLDown;
    if (std::abs(sticks[0].x) > 8000 || std::abs(sticks[0].y) > 8000 || std::abs(sticks[1].x) > 8000 ||
        std::abs(sticks[1].y) > 8000)
        gMode = MODE_PAD;
    pad->sticks[0] = sticks[0];
    pad->sticks[1] = sticks[1];
    pad->buttons = b;
    if (gSwallow) {
        pad->old |= b;
        gSwallow = false;
    }
}

size_t hidGetTouchScreenStates(HidTouchScreenState *states, size_t count) {
    if (count == 0)
        return 0;
    states[0].count = gMouseDown ? 1 : 0;
    states[0].touches[0].x = std::clamp(gMouseX, 0, 1279);
    states[0].touches[0].y = std::clamp(gMouseY, 0, 719);
    return 1;
}

u64 armGetSystemTick() {
    return SDL_GetPerformanceCounter();
}

u64 armGetSystemTickFreq() {
    return SDL_GetPerformanceFrequency();
}

u64 armTicksToNs(u64 ticks) {
    return u64(double(ticks) * 1e9 / double(SDL_GetPerformanceFrequency()));
}

bool appletMainLoop() {
    return !gQuit;
}

#ifdef __ANDROID__
// Unpacks the data bundled in the APK's assets (listed in data/files.txt) to internal storage
// once per version, since the app reads it with normal file calls.
static void unpackAssets(const std::string &dir) {
    std::string stamp = dir + ".version";
    std::ifstream in(stamp);
    std::string have;
    std::getline(in, have);
    if (have == APP_VERSION)
        return;
    SDL_RWops *list = SDL_RWFromFile("data/files.txt", "rb");
    if (!list)
        return;
    std::string names(size_t(SDL_RWsize(list)), '\0');
    SDL_RWread(list, names.data(), 1, names.size());
    SDL_RWclose(list);
    std::istringstream ss(names);
    std::string name;
    while (std::getline(ss, name)) {
        if (name.empty())
            continue;
        SDL_RWops *f = SDL_RWFromFile(("data/" + name).c_str(), "rb");
        if (!f)
            continue;
        std::string body(size_t(SDL_RWsize(f)), '\0');
        SDL_RWread(f, body.data(), 1, body.size());
        SDL_RWclose(f);
        std::string out = dir + name;
        for (size_t i = out.find('/', 1); i != std::string::npos; i = out.find('/', i + 1))
            mkdir(out.substr(0, i).c_str(), 0777);
        std::ofstream(out, std::ios::binary | std::ios::trunc) << body;
    }
    std::ofstream(stamp, std::ios::trunc) << APP_VERSION << '\n';
}
#endif

Result romfsInit() {
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#ifdef __ANDROID__
    // Progress and downloads in the app's internal storage; the bundled data unpacked next to them.
    std::string internal = SDL_AndroidGetInternalStoragePath() ? SDL_AndroidGetInternalStoragePath() : ".";
    if (chdir(internal.c_str()) != 0)
        std::fprintf(stderr, "Cannot use %s\n", internal.c_str());
    gRomfs = internal + "/romfs/";
    mkdir(gRomfs.c_str(), 0777);
    unpackAssets(gRomfs);
    return 0;
#endif
    if (char *b = SDL_GetBasePath()) {
        gBase = b;
        SDL_free(b);
    }
    // Packaged builds keep the data next to the executable; TARKOV_DATA points a dev build elsewhere.
    const char *env = SDL_getenv("TARKOV_DATA");
    gRomfs = env && *env ? std::string(env) : gBase + "data";
    if (gRomfs.back() != '/' && gRomfs.back() != '\\')
        gRomfs += '/';
    // Downloads, progress and settings go to the user's data folder, which becomes the working
    // directory so the Switch code's relative APP_DIR paths land there.
    if (char *p = SDL_GetPrefPath("", "TarkovHideout")) {
        if (chdir(p) != 0)
            std::fprintf(stderr, "Cannot use the data folder %s\n", p);
        SDL_free(p);
    }
    return 0;
}

const std::string &nxRomfsRoot() {
    return gRomfs;
}

Result plGetSharedFontByType(PlFontData *font, PlSharedFontType) {
    if (gFont.empty()) {
        // SDL's file calls also read Android assets, where gBase is empty.
        for (const std::string &p : {gBase + "font.otf", gRomfs + "../font.otf"}) {
            SDL_RWops *f = SDL_RWFromFile(p.c_str(), "rb");
            if (!f)
                continue;
            gFont.resize(size_t(SDL_RWsize(f)));
            SDL_RWread(f, gFont.data(), 1, gFont.size());
            SDL_RWclose(f);
            break;
        }
        if (gFont.empty()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tarkov Hideout",
                                     ("font.otf is missing next to the program in " + gBase).c_str(), nullptr);
            return 1;
        }
    }
    font->address = gFont.data();
    font->size = gFont.size();
    return 0;
}

float nxWheel() {
    return gWheel;
}

int nxTabKey() {
    return gTabKey;
}

bool nxPointer(int &x, int &y) {
    x = gMouseX;
    y = gMouseY;
    return gMouseIn;
}

bool nxTouchCancelled() {
    bool c = gTouchCancelled;
    gTouchCancelled = false;
    return c;
}

const std::string &nxButtonLabel(const std::string &button) {
    // On a touch screen the hints are buttons named by what they do.
    static const std::map<std::string, std::string> kTouch = {
        {"A", "OK"},         {"B", "Back"},       {"L", "<"},      {"R", ">"},     {"ZL/ZR", "Prev/Next"},
        {"ZL", "Prev"},      {"ZR", "Next"},      {"+", "Quit"},   {"-", "Full"},  {"R3", "Style"},
        {"</>", "←/→"}, {"^/v", "↑/↓"}, {">", "→"}, {"A/X", "+/-"}, {"R-stick", "Drag"},
    };
    static const std::map<std::string, std::string> kKeys = {
        {"A", "Enter"},      {"B", "Esc"},        {"L", "Q"},     {"R", "E"},     {"ZL/ZR", "PgUp/PgDn"},
        {"ZL", "PgUp"},      {"ZR", "PgDn"},      {"+", "Ctrl+Q"}, {"-", "F"},    {"R3", "R"},
        {"</>", "←/→"}, {"^/v", "↑/↓"}, {">", "→"}, {"A/X", "Wheel"}, {"R-stick", "Wheel"},
    };
    if (gMode == MODE_PAD)
        return button;
    const auto &names = gMode == MODE_TOUCH ? kTouch : kKeys;
    auto it = names.find(button);
    return it == names.end() ? button : it->second;
}

float nxUiScale() {
    if (!gRenderer)
        return 1;
    int w, h;
    if (SDL_GetRendererOutputSize(gRenderer, &w, &h) != 0)
        return 1;
    return std::max(0.25f, std::min(w / 1280.f, h / 720.f));
}

void nxWindowCreated(SDL_Window *window, SDL_Renderer *renderer) {
    gWindow = window;
    gRenderer = renderer;
    SDL_RenderSetLogicalSize(renderer, 1280, 720);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (SDL_Surface *icon = IMG_Load((gBase + "icon.png").c_str())) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
#ifdef __ANDROID__
    return;
#endif
    std::ifstream in(kWindowFile);
    int x, y, w, h, max;
    if (in >> x >> y >> w >> h >> max) {
        // Only restore a position that is still on one of the screens.
        bool onScreen = false;
        for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
            SDL_Rect r;
            if (SDL_GetDisplayBounds(i, &r) == 0 && x + 40 > r.x && x < r.x + r.w - 40 && y + 20 > r.y &&
                y < r.y + r.h - 40)
                onScreen = true;
        }
        SDL_SetWindowSize(window, std::max(w, 640), std::max(h, 360));
        if (onScreen)
            SDL_SetWindowPosition(window, x, y);
        else
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (max)
            SDL_MaximizeWindow(window);
    } else {
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    SDL_SetWindowMinimumSize(window, 640, 360);
}

// ---------------------------------------------------------------- text box

namespace {

struct BoxText {
    TTF_Font *font = nullptr;
    float scale = 1;

    void draw(int x, int y, const std::string &s, SDL_Color c) {
        if (s.empty() || !font)
            return;
        SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
        if (!surf)
            return;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(gRenderer, surf);
        SDL_FRect dst{float(x), float(y), surf->w / scale, surf->h / scale};
        SDL_RenderCopyF(gRenderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        SDL_FreeSurface(surf);
    }

    int width(const std::string &s) {
        int w = 0, h = 0;
        if (font && !s.empty())
            TTF_SizeUTF8(font, s.c_str(), &w, &h);
        return int(w / scale);
    }
};

void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r{x, y, w, h};
    SDL_SetRenderDrawColor(gRenderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(gRenderer, &r);
}

std::string clean(const std::string &s, bool numeric) {
    std::string out;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t')
            continue;
        if (numeric && (c < '0' || c > '9'))
            continue;
        out += c;
    }
    return out;
}

} // namespace

Result swkbdShow(SwkbdConfig *c, char *out, size_t outSize) {
    if (!gRenderer)
        return 1;
    BoxText txt;
    txt.scale = nxUiScale();
    if (!gFont.empty())
        txt.font = TTF_OpenFontRW(SDL_RWFromConstMem(gFont.data(), gFont.size()), 1, int(24 * txt.scale + 0.5f));
    // Tokens stay hidden on screen apart from their end, in case the window is being streamed.
    std::string lowerHeader = c->header;
    std::transform(lowerHeader.begin(), lowerHeader.end(), lowerHeader.begin(), ::tolower);
    bool secret = lowerHeader.find("token") != std::string::npos;

    std::string text = c->initial;
    bool done = false, ok = false;
    // High on the screen so an on-screen keyboard leaves it visible; OK and Cancel are tappable.
    const int bx = 190, by = 70, bw = 900, bh = 230;
    const SDL_Rect okR{bx + bw - 288, by + bh - 66, 120, 46}, cancelR{bx + bw - 156, by + bh - 66, 128, 46};
    SDL_StartTextInput();
    while (!done) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 250)) {
            do {
                switch (e.type) {
                case SDL_QUIT:
                    gQuit = true;
                    done = true;
                    break;
                case SDL_TEXTINPUT:
                    if (text.size() < c->maxLen)
                        text += clean(e.text.text, c->numeric);
                    break;
                case SDL_KEYDOWN: {
                    SDL_Keycode k = e.key.keysym.sym;
                    bool ctl = e.key.keysym.mod & KMOD_CTRL;
                    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        ok = done = true;
                    } else if (k == SDLK_ESCAPE) {
                        done = true;
                    } else if (k == SDLK_BACKSPACE && !text.empty()) {
                        if (ctl) {
                            text.clear();
                        } else {
                            size_t n = text.size() - 1;
                            while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
                                n--;
                            text.resize(n);
                        }
                    } else if (ctl && k == SDLK_v) {
                        if (char *clip = SDL_GetClipboardText()) {
                            text += clean(clip, c->numeric);
                            SDL_free(clip);
                            if (text.size() > c->maxLen)
                                text.resize(c->maxLen);
                        }
                    }
                    break;
                }
                case SDL_CONTROLLERBUTTONDOWN:
                    if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A)
                        ok = done = true;
                    else if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B)
                        done = true;
                    break;
                case SDL_MOUSEBUTTONDOWN: {
                    SDL_Point pt{e.button.x, e.button.y};
                    if (e.button.button == SDL_BUTTON_RIGHT || SDL_PointInRect(&pt, &cancelR))
                        done = true;
                    else if (SDL_PointInRect(&pt, &okR))
                        ok = done = true;
                    else if (SDL_IsTextInputShown() == SDL_FALSE)
                        SDL_StartTextInput();  // tapping the box brings the touch keyboard back
                    break;
                }
                default: break;
                }
            } while (!done && SDL_PollEvent(&e));
        }

        float s = nxUiScale();
        if (std::fabs(s - txt.scale) > 0.01f && !gFont.empty()) {
            if (txt.font)
                TTF_CloseFont(txt.font);
            txt.scale = s;
            txt.font = TTF_OpenFontRW(SDL_RWFromConstMem(gFont.data(), gFont.size()), 1, int(24 * s + 0.5f));
        }
        SDL_RenderSetClipRect(gRenderer, nullptr);
        SDL_SetRenderDrawColor(gRenderer, col::bg.r, col::bg.g, col::bg.b, 255);
        SDL_RenderClear(gRenderer);
        fillRect(bx, by, bw, bh, col::panel);
        fillRect(bx, by, bw, 3, col::accent);
        txt.draw(bx + 28, by + 22, c->header, col::accent);
        txt.draw(bx + 28, by + 56, c->guide, col::dim);
        fillRect(bx + 28, by + 100, bw - 56, 48, col::cell);
        fillRect(bx + 28, by + 146, bw - 56, 2, col::accent);
        std::string shown = text;
        if (secret && text.size() > 4)
            shown = std::string(std::min<size_t>(text.size() - 4, 40), '*') + text.substr(text.size() - 4);
        // Keep the end of long text in view.
        while (txt.width(shown) > bw - 90 && !shown.empty())
            shown.erase(0, 1);
        txt.draw(bx + 40, by + 110, shown, col::text);
        if (SDL_GetTicks() / 500 % 2 == 0)
            fillRect(bx + 42 + txt.width(shown), by + 110, 2, 28, col::text);
#ifndef __ANDROID__
        txt.draw(bx + 28, by + 176, "Ctrl+V  Paste     Ctrl+Backspace  Clear", col::dim);
#endif
        fillRect(okR.x, okR.y, okR.w, okR.h, col::accent);
        txt.draw(okR.x + (okR.w - txt.width("OK")) / 2, okR.y + 9, "OK", col::bg);
        fillRect(cancelR.x, cancelR.y, cancelR.w, cancelR.h, col::cell);
        txt.draw(cancelR.x + (cancelR.w - txt.width("Cancel")) / 2, cancelR.y + 9, "Cancel", col::text);
        SDL_RenderPresent(gRenderer);
    }
    SDL_StopTextInput();
    if (txt.font)
        TTF_CloseFont(txt.font);
    // The keys that closed the box must not also act in the app.
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_KEYDOWN, SDL_TEXTINPUT);
    gInjected = 0;
    gMouseDown = false;
    gSwallow = true;
    if (!ok)
        return 1;
    std::snprintf(out, outSize, "%s", text.c_str());
    return 0;
}
