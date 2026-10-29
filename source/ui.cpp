#include "ui.h"

#include "platform.h"

#include <algorithm>

#include <cmath>
#include <vector>

static const int kAtlasCols = 32;
static const int kPageIcons = kAtlasCols * kAtlasCols;
static const int kIconPx = 64;

static const int kFontPx[FS_COUNT] = {20, 24, 30};

bool Ui::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return false;
    if (TTF_Init() < 0)
        return false;
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    window = SDL_CreateWindow("Tarkov Hideout", 0, 0, 1280, 720, NX_WINDOW_FLAGS);
    if (!window)
        return false;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        return false;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    nxWindowCreated(window, renderer);

    // The system's shared font avoids shipping a font file.
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)))
        return false;
    fontData = font.address;
    fontSize = font.size;
    for (int i = 0; i < FS_COUNT; i++) {
        SDL_RWops *rw = SDL_RWFromConstMem(fontData, fontSize);
        fonts[i] = TTF_OpenFontRW(rw, 1, kFontPx[i]);
        if (!fonts[i])
            return false;
        lineH[i] = TTF_FontLineSkip(fonts[i]);
    }
    return true;
}

// The layout follows the window shape and zoom, and a window with more pixels than the layout
// renders text from bigger fonts so it stays sharp.
void Ui::updateScale() {
    int lw, lh;
    nxLayoutSize(lw, lh);
    if (lw != layoutW || lh != layoutH) {
        layoutW = lw;
        layoutH = lh;
        SDL_RenderSetLogicalSize(renderer, lw, lh);
    }
    float s = nxUiScale();
    if (std::fabs(s - scale) < 0.01f)
        return;
    scale = s;
    for (auto &[k, c] : cache)
        SDL_DestroyTexture(c.tex);
    cache.clear();
    widths.clear();
    fits.clear();
    for (auto &f : drawFonts) {
        if (f)
            TTF_CloseFont(f);
        f = nullptr;
    }
    if (std::fabs(scale - 1) < 0.01f)
        return;
    for (int i = 0; i < FS_COUNT; i++)
        drawFonts[i] = TTF_OpenFontRW(SDL_RWFromConstMem(fontData, fontSize), 1, int(kFontPx[i] * scale + 0.5f));
}

void Ui::shutdown() {
    setIcons({});
    for (auto &[k, c] : cache)
        SDL_DestroyTexture(c.tex);
    cache.clear();
    widths.clear();
    fits.clear();
    for (auto &f : fonts)
        if (f)
            TTF_CloseFont(f);
    for (auto &f : drawFonts)
        if (f)
            TTF_CloseFont(f);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

void Ui::clear() {
    updateScale();
    SDL_SetRenderDrawColor(renderer, col::bg.r, col::bg.g, col::bg.b, 255);
    SDL_RenderClear(renderer);
}

void Ui::present() {
    SDL_RenderPresent(renderer);
}

void Ui::fill(int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(renderer, &r);
}

void Ui::rect(int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_RenderDrawRect(renderer, &r);
}

void Ui::vgradient(int x, int y, int w, int h, SDL_Color top, SDL_Color bottom) {
    SDL_Vertex v[4] = {
        {{float(x), float(y)}, top, {0, 0}},
        {{float(x + w), float(y)}, top, {0, 0}},
        {{float(x + w), float(y + h)}, bottom, {0, 0}},
        {{float(x), float(y + h)}, bottom, {0, 0}},
    };
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
}

void Ui::disc(float cx, float cy, float r, SDL_Color c) {
    const int max = 20;
    int n = r < 5 ? 8 : r < 9 ? 12 : max;  // small dots don't need a smooth edge
    SDL_Vertex v[max + 1];
    int idx[max * 3];
    v[0] = {{cx, cy}, c, {0, 0}};
    for (int i = 0; i < n; i++) {
        float a = float(i) * 2.f * float(M_PI) / n;
        v[i + 1] = {{cx + r * std::cos(a), cy + r * std::sin(a)}, c, {0, 0}};
        idx[i * 3] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = (i + 1) % n + 1;
    }
    SDL_RenderGeometry(renderer, nullptr, v, n + 1, idx, n * 3);
}

void Ui::progress(int x, int y, int w, int h, float frac, SDL_Color c) {
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
    fill(x, y, w, h, col::cell);
    if (frac > 0)
        fill(x, y, std::max(1, int(w * frac)), h, c);
}

static bool roundButton(const std::string &b) {
    return b == "A" || b == "B" || b == "X" || b == "Y";
}

int Ui::hintsWidth(const std::string &name, const std::string &label) {
    const std::string &button = nxButtonLabel(name);
    int bw = roundButton(button) ? 26 : measure(button, FS_SMALL) + 16;
    return bw + 8 + measure(label, FS_SMALL);
}

int Ui::hint(int x, int y, const std::string &name, const std::string &label) {
    const std::string &button = nxButtonLabel(name);
    int bw;
    if (roundButton(button)) {
        bw = 26;
        disc(x + 13.f, y + 13.f, 13.f, col::text);
    } else {
        bw = measure(button, FS_SMALL) + 16;
        fill(x, y + 1, bw, 24, col::text);
    }
    int tw = measure(button, FS_SMALL);
    text(x + (bw - tw) / 2, y + 13 - lineH[FS_SMALL] / 2, button, FS_SMALL, col::bg);
    text(x + bw + 8, y + 13 - lineH[FS_SMALL] / 2, label, FS_SMALL, col::dim);
    return bw + 8 + measure(label, FS_SMALL);
}

void Ui::clip(const SDL_Rect *r) {
    SDL_RenderSetClipRect(renderer, r);
}

SDL_Texture *Ui::texture(SDL_Surface *surf) {
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    return tex;
}

Uint32 Ui::textureFormat() const {
    SDL_RendererInfo info;
    if (renderer && SDL_GetRendererInfo(renderer, &info) == 0 && info.num_texture_formats > 0)
        return info.texture_formats[0];
    return SDL_PIXELFORMAT_ARGB8888;
}

SDL_Texture *Ui::loadImage(const std::string &path, int *w, int *h) {
    SDL_Surface *surf = IMG_Load(path.c_str());
    if (!surf)
        return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    if (w)
        *w = surf->w;
    if (h)
        *h = surf->h;
    SDL_FreeSurface(surf);
    return tex;
}

bool Ui::loadImageTiles(const std::string &path, int tile, std::vector<SDL_Texture *> &out, int &w, int &h) {
    SDL_Surface *surf = IMG_Load(path.c_str());
    if (!surf)
        return false;
    w = surf->w;
    h = surf->h;
    int bpp = surf->format->BytesPerPixel;
    bool ok = true;
    for (int y = 0; y < h && ok; y += tile)
        for (int x = 0; x < w && ok; x += tile) {
            int tw = std::min(tile, w - x), th = std::min(tile, h - y);
            SDL_Texture *t = SDL_CreateTexture(renderer, surf->format->format, SDL_TEXTUREACCESS_STATIC, tw, th);
            ok = t && SDL_UpdateTexture(t, nullptr, (Uint8 *)surf->pixels + y * surf->pitch + x * bpp, surf->pitch) == 0;
            if (t)
                SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
            out.push_back(t);
        }
    SDL_FreeSurface(surf);
    if (!ok) {
        for (SDL_Texture *t : out)
            if (t)
                SDL_DestroyTexture(t);
        out.clear();
    }
    return ok;
}

void Ui::setIcons(const std::vector<SDL_Texture *> &pages) {
    for (SDL_Texture *t : icons)
        SDL_DestroyTexture(t);
    icons = pages;
}

void Ui::icon(int idx, int x, int y, int size) {
    fill(x, y, size, size, col::cell);
    int page = idx / kPageIcons, cell = idx % kPageIcons;
    if (idx >= 0 && page < (int)icons.size() && icons[page]) {
        SDL_Rect src{(cell % kAtlasCols) * kIconPx, (cell / kAtlasCols) * kIconPx, kIconPx, kIconPx};
        SDL_Rect dst{x + 1, y + 1, size - 2, size - 2};
        SDL_RenderCopy(renderer, icons[page], &src, &dst);
    }
    rect(x, y, size, size, col::line);
}

void Ui::image(SDL_Texture *tex, const SDL_Rect *src, const SDL_FRect *dst) {
    SDL_RenderCopyF(renderer, tex, src, dst);
}

Ui::Cached *Ui::get(const std::string &s, FontSize fs, SDL_Color c) {
    std::string key;
    key.reserve(s.size() + 8);
    key += char('0' + fs);
    key += char(c.r);
    key += char(c.g);
    key += char(c.b);
    key += s;
    auto it = cache.find(key);
    if (it != cache.end()) {
        it->second.lastUsed = frame;
        return &it->second;
    }
    TTF_Font *font = drawFonts[fs] ? drawFonts[fs] : fonts[fs];
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!surf)
        return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    Cached entry{tex, surf->w / scale, surf->h / scale, frame};
    SDL_FreeSurface(surf);
    if (!tex)
        return nullptr;
    return &cache.emplace(key, entry).first->second;
}

int Ui::measure(const std::string &s, FontSize fs) {
    if (s.empty())
        return 0;
    // Text shaping is slow on the Switch and the same strings are measured every frame.
    std::string key = char('0' + fs) + s;
    auto it = widths.find(key);
    if (it != widths.end())
        return it->second;
    int w = 0, h = 0;
    TTF_SizeUTF8(fonts[fs], s.c_str(), &w, &h);
    if (widths.size() > 8000)
        widths.clear();
    widths.emplace(std::move(key), w);
    return w;
}

std::vector<std::string> Ui::wrap(const std::string &s, FontSize fs, int w) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && measure(cand, fs) > w) {
                lines.push_back(line);
                line = word;
            } else {
                line = cand;
            }
            if (sp == std::string::npos)
                break;
            i = sp + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return lines;
}

std::string Ui::fit(const std::string &s, FontSize fs, int maxW) {
    if (maxW <= 0 || measure(s, fs) <= maxW)
        return s;
    std::string key = std::to_string(maxW) + char('0' + fs) + s;
    auto it = fits.find(key);
    if (it != fits.end())
        return it->second;
    if (fits.size() > 4000)
        fits.clear();
    std::string &out = fits[key];
    out = cutToFit(s, fs, maxW);
    return out;
}

std::string Ui::cutToFit(const std::string &s, FontSize fs, int maxW) {
    std::string cut = s;
    while (!cut.empty()) {
        // Drop one UTF-8 code point.
        size_t n = cut.size() - 1;
        while (n > 0 && (static_cast<unsigned char>(cut[n]) & 0xC0) == 0x80)
            n--;
        cut.resize(n);
        if (measure(cut + "...", fs) <= maxW)
            return cut + "...";
    }
    return "";
}

int Ui::text(int x, int y, const std::string &s, FontSize fs, SDL_Color c, int maxW) {
    if (s.empty())
        return 0;
    Cached *t = get(fit(s, fs, maxW), fs, c);
    if (!t)
        return 0;
    SDL_FRect dst{float(x), float(y), t->w, t->h};
    SDL_RenderCopyF(renderer, t->tex, nullptr, &dst);
    return int(t->w + 0.5f);
}

int Ui::textRight(int right, int y, const std::string &s, FontSize fs, SDL_Color c) {
    int w = measure(s, fs);
    return text(right - w, y, s, fs, c);
}

void Ui::endFrame() {
    frame++;
    if (frame % 120 != 0 || cache.size() < 600)
        return;
    std::vector<std::string> stale;
    for (auto &[k, c] : cache)
        if (frame - c.lastUsed > 120)
            stale.push_back(k);
    for (auto &k : stale) {
        SDL_DestroyTexture(cache[k].tex);
        cache.erase(k);
    }
}

static SDL_Color rgb(int r, int g, int b, int a = 255) { return SDL_Color{Uint8(r), Uint8(g), Uint8(b), Uint8(a)}; }

const std::vector<Theme> &themes() {
    // bg, bgTop, panel, panelHi, cell, line, text, dim, faint, accent, good, warn, bad, info, violet, shadow
    static const std::vector<Theme> list = {
        {"Tarkov", rgb(14, 15, 13), rgb(24, 25, 21), rgb(27, 28, 24), rgb(46, 46, 38), rgb(36, 37, 32), rgb(58, 58, 50),
         rgb(230, 226, 210), rgb(146, 143, 128), rgb(92, 90, 80), rgb(205, 178, 110), rgb(126, 184, 98),
         rgb(222, 156, 72), rgb(212, 94, 82), rgb(108, 164, 214), rgb(170, 128, 214), rgb(0, 0, 0, 150)},
        {"OLED", rgb(0, 0, 0), rgb(0, 0, 0), rgb(12, 12, 12), rgb(32, 32, 30), rgb(20, 20, 20), rgb(46, 46, 44),
         rgb(236, 236, 232), rgb(150, 150, 146), rgb(90, 90, 88), rgb(205, 178, 110), rgb(120, 200, 100),
         rgb(230, 160, 70), rgb(230, 90, 80), rgb(100, 170, 230), rgb(175, 130, 225), rgb(0, 0, 0, 180)},
        {"Catppuccin Mocha", rgb(24, 24, 37), rgb(17, 17, 27), rgb(30, 30, 46), rgb(69, 71, 90), rgb(49, 50, 68),
         rgb(69, 71, 90), rgb(205, 214, 244), rgb(166, 173, 200), rgb(108, 112, 134), rgb(203, 166, 247),
         rgb(166, 227, 161), rgb(250, 179, 135), rgb(243, 139, 168), rgb(137, 180, 250), rgb(180, 190, 254),
         rgb(17, 17, 27, 170)},
        {"Catppuccin Latte", rgb(230, 233, 239), rgb(220, 224, 232), rgb(239, 241, 245), rgb(204, 208, 218),
         rgb(220, 224, 232), rgb(172, 176, 190), rgb(76, 79, 105), rgb(108, 111, 133), rgb(156, 160, 176),
         rgb(136, 57, 239), rgb(64, 160, 43), rgb(254, 100, 11), rgb(210, 15, 57), rgb(30, 102, 245),
         rgb(114, 135, 253), rgb(239, 241, 245, 210)},
        {"Light", rgb(236, 236, 232), rgb(248, 248, 245), rgb(250, 250, 247), rgb(222, 222, 212), rgb(238, 238, 232),
         rgb(205, 205, 195), rgb(30, 30, 28), rgb(95, 95, 88), rgb(150, 150, 140), rgb(166, 124, 30),
         rgb(46, 140, 50), rgb(200, 110, 20), rgb(200, 50, 45), rgb(30, 110, 190), rgb(120, 70, 190),
         rgb(255, 255, 255, 210)},
        {"Nord", rgb(46, 52, 64), rgb(39, 44, 54), rgb(59, 66, 82), rgb(76, 86, 106), rgb(67, 76, 94), rgb(76, 86, 106),
         rgb(236, 239, 244), rgb(176, 186, 202), rgb(120, 130, 150), rgb(136, 192, 208), rgb(163, 190, 140),
         rgb(208, 135, 112), rgb(191, 97, 106), rgb(129, 161, 193), rgb(180, 142, 173), rgb(30, 34, 42, 170)},
        {"Dracula", rgb(33, 34, 44), rgb(25, 26, 33), rgb(40, 42, 54), rgb(68, 71, 90), rgb(52, 55, 70), rgb(68, 71, 90),
         rgb(248, 248, 242), rgb(170, 175, 200), rgb(98, 114, 164), rgb(189, 147, 249), rgb(80, 250, 123),
         rgb(255, 184, 108), rgb(255, 85, 85), rgb(139, 233, 253), rgb(255, 121, 198), rgb(20, 20, 28, 170)},
        {"Gruvbox", rgb(29, 32, 33), rgb(22, 24, 25), rgb(40, 40, 40), rgb(80, 73, 69), rgb(50, 48, 47), rgb(80, 73, 69),
         rgb(235, 219, 178), rgb(189, 174, 147), rgb(124, 111, 100), rgb(250, 189, 47), rgb(184, 187, 38),
         rgb(254, 128, 25), rgb(251, 73, 52), rgb(131, 165, 152), rgb(211, 134, 155), rgb(0, 0, 0, 160)},
        {"Solarized", rgb(0, 43, 54), rgb(0, 36, 46), rgb(7, 54, 66), rgb(20, 75, 90), rgb(10, 60, 72), rgb(30, 85, 100),
         rgb(238, 232, 213), rgb(147, 161, 161), rgb(88, 110, 117), rgb(181, 137, 0), rgb(133, 153, 0),
         rgb(203, 75, 22), rgb(220, 50, 47), rgb(38, 139, 210), rgb(108, 113, 196), rgb(0, 20, 26, 170)},
        {"Tokyo Night", rgb(26, 27, 38), rgb(22, 22, 30), rgb(31, 35, 53), rgb(52, 59, 88), rgb(36, 40, 59),
         rgb(59, 66, 97), rgb(192, 202, 245), rgb(154, 165, 206), rgb(86, 95, 137), rgb(122, 162, 247),
         rgb(158, 206, 106), rgb(255, 158, 100), rgb(247, 118, 142), rgb(125, 207, 255), rgb(187, 154, 247),
         rgb(16, 16, 22, 170)},
        {"Rose Pine", rgb(25, 23, 36), rgb(21, 19, 31), rgb(31, 29, 46), rgb(64, 61, 82), rgb(38, 35, 58),
         rgb(64, 61, 82), rgb(224, 222, 244), rgb(144, 140, 170), rgb(110, 106, 134), rgb(246, 193, 119),
         rgb(156, 207, 216), rgb(235, 188, 186), rgb(235, 111, 146), rgb(86, 148, 175), rgb(196, 167, 231),
         rgb(16, 14, 24, 170)},
        {"Woodland", rgb(18, 22, 16), rgb(24, 30, 22), rgb(28, 35, 25), rgb(48, 60, 42), rgb(36, 45, 32),
         rgb(60, 74, 52), rgb(222, 228, 206), rgb(150, 160, 130), rgb(92, 104, 80), rgb(176, 196, 96),
         rgb(126, 190, 98), rgb(222, 170, 72), rgb(212, 94, 82), rgb(120, 170, 200), rgb(170, 140, 210),
         rgb(0, 0, 0, 150)},
        {"High contrast", rgb(0, 0, 0), rgb(0, 0, 0), rgb(0, 0, 0), rgb(48, 48, 0), rgb(26, 26, 26), rgb(255, 255, 255),
         rgb(255, 255, 255), rgb(225, 225, 225), rgb(165, 165, 165), rgb(255, 215, 0), rgb(0, 255, 100),
         rgb(255, 160, 0), rgb(255, 70, 70), rgb(80, 200, 255), rgb(220, 140, 255), rgb(0, 0, 0, 210)},
    };
    return list;
}

int themeIndex(const std::string &name) {
    const auto &list = themes();
    for (int i = 0; i < (int)list.size(); i++)
        if (name == list[i].name)
            return i;
    return 0;
}

void applyTheme(int idx) {
    const auto &list = themes();
    const Theme &t = list[idx >= 0 && idx < (int)list.size() ? idx : 0];
    col::bg = t.bg;
    col::bgTop = t.bgTop;
    col::panel = t.panel;
    col::panelHi = t.panelHi;
    col::cell = t.cell;
    col::line = t.line;
    col::text = t.text;
    col::dim = t.dim;
    col::faint = t.faint;
    col::accent = t.accent;
    col::good = t.good;
    col::warn = t.warn;
    col::bad = t.bad;
    col::info = t.info;
    col::violet = t.violet;
    col::shadow = t.shadow;
}
