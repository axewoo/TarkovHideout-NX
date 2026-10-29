#include "ui.h"

#include <switch.h>

#include <vector>

static const int kFontPx[FS_COUNT] = {20, 24, 30};

bool Ui::init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return false;
    if (TTF_Init() < 0)
        return false;
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
