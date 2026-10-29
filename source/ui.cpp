#include "ui.h"

#include <switch.h>

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
    window = SDL_CreateWindow("Tarkov Hideout", 0, 0, 1280, 720, 0);
    if (!window)
        return false;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        return false;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // The system's shared font avoids shipping a font file.
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard)))
        return false;
    for (int i = 0; i < FS_COUNT; i++) {
        SDL_RWops *rw = SDL_RWFromConstMem(font.address, font.size);
        fonts[i] = TTF_OpenFontRW(rw, 1, kFontPx[i]);
        if (!fonts[i])
            return false;
        lineH[i] = TTF_FontLineSkip(fonts[i]);
    }
    return true;
}

void Ui::shutdown() {
    setIcons({});
    for (auto &[k, c] : cache)
        SDL_DestroyTexture(c.tex);
    cache.clear();
    for (auto &f : fonts)
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
    const int n = 20;
    SDL_Vertex v[n + 1];
    int idx[n * 3];
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

int Ui::hintsWidth(const std::string &button, const std::string &label) {
    int bw = roundButton(button) ? 26 : measure(button, FS_SMALL) + 16;
    return bw + 8 + measure(label, FS_SMALL);
}

int Ui::hint(int x, int y, const std::string &button, const std::string &label) {
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
    SDL_Surface *surf = TTF_RenderUTF8_Blended(fonts[fs], s.c_str(), c);
    if (!surf)
        return nullptr;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    Cached entry{tex, surf->w, surf->h, frame};
    SDL_FreeSurface(surf);
    if (!tex)
        return nullptr;
    return &cache.emplace(key, entry).first->second;
}

int Ui::measure(const std::string &s, FontSize fs) {
    int w = 0, h = 0;
    if (s.empty())
        return 0;
    TTF_SizeUTF8(fonts[fs], s.c_str(), &w, &h);
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
    SDL_Rect dst{x, y, t->w, t->h};
    SDL_RenderCopy(renderer, t->tex, nullptr, &dst);
    return t->w;
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
