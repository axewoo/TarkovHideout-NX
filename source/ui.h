#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <unordered_map>

namespace col {
constexpr SDL_Color bg{18, 19, 16, 255};
constexpr SDL_Color panel{30, 31, 27, 255};
constexpr SDL_Color panelHi{52, 52, 44, 255};
constexpr SDL_Color line{60, 60, 52, 255};
constexpr SDL_Color text{226, 222, 205, 255};
constexpr SDL_Color dim{148, 145, 130, 255};
constexpr SDL_Color accent{196, 170, 105, 255};
constexpr SDL_Color good{122, 178, 96, 255};
constexpr SDL_Color warn{214, 150, 70, 255};
constexpr SDL_Color bad{204, 92, 80, 255};
} // namespace col

enum FontSize { FS_SMALL, FS_BODY, FS_TITLE, FS_COUNT };

class Ui {
public:
    bool init();
    void shutdown();

    void clear();
    void present();
    void fill(int x, int y, int w, int h, SDL_Color c);
    void rect(int x, int y, int w, int h, SDL_Color c);

    // Draws text, truncating with "..." past maxW. Returns drawn width.
    int text(int x, int y, const std::string &s, FontSize fs, SDL_Color c, int maxW = 0);
    int textRight(int right, int y, const std::string &s, FontSize fs, SDL_Color c);
    int measure(const std::string &s, FontSize fs);
    int lineHeight(FontSize fs) const { return lineH[fs]; }

    // Drops cached textures not used for a while.
    void endFrame();

private:
    struct Cached {
        SDL_Texture *tex;
        int w, h;
        unsigned lastUsed;
    };
    Cached *get(const std::string &s, FontSize fs, SDL_Color c);
    std::string fit(const std::string &s, FontSize fs, int maxW);

    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    TTF_Font *fonts[FS_COUNT] = {};
    int lineH[FS_COUNT] = {};
    std::unordered_map<std::string, Cached> cache;
    unsigned frame = 0;
};
