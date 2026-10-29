#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace col {
constexpr SDL_Color bg{14, 15, 13, 255};
constexpr SDL_Color bgTop{24, 25, 21, 255};
constexpr SDL_Color panel{27, 28, 24, 255};
constexpr SDL_Color panelHi{46, 46, 38, 255};
constexpr SDL_Color cell{36, 37, 32, 255};
constexpr SDL_Color line{58, 58, 50, 255};
constexpr SDL_Color text{230, 226, 210, 255};
constexpr SDL_Color dim{146, 143, 128, 255};
constexpr SDL_Color faint{92, 90, 80, 255};
constexpr SDL_Color accent{205, 178, 110, 255};
constexpr SDL_Color good{126, 184, 98, 255};
constexpr SDL_Color warn{222, 156, 72, 255};
constexpr SDL_Color bad{212, 94, 82, 255};
constexpr SDL_Color info{108, 164, 214, 255};
constexpr SDL_Color violet{170, 128, 214, 255};
constexpr SDL_Color shadow{0, 0, 0, 150};
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
    void vgradient(int x, int y, int w, int h, SDL_Color top, SDL_Color bottom);
    void disc(float cx, float cy, float r, SDL_Color c);
    void progress(int x, int y, int w, int h, float frac, SDL_Color c);
    // A button glyph ("A", "ZL", ...) followed by a label; returns the width used.
    int hint(int x, int y, const std::string &button, const std::string &label);
    int hintsWidth(const std::string &button, const std::string &label);
    void clip(const SDL_Rect *r);

    // Images. Icon atlases are pages of 32 x 32 cells of 64 px; index / 1024 is the page.
    SDL_Texture *loadImage(const std::string &path, int *w = nullptr, int *h = nullptr);
    // Loads a large image as a grid of tile x tile textures (row-major), for images past the GPU's size limit.
    bool loadImageTiles(const std::string &path, int tile, std::vector<SDL_Texture *> &out, int &w, int &h);
    void setIcons(const std::vector<SDL_Texture *> &pages);
    bool hasIcons() const { return !icons.empty(); }
    // Draws an item icon in a square cell; idx < 0 draws an empty cell.
    void icon(int idx, int x, int y, int size);
    void image(SDL_Texture *tex, const SDL_Rect *src, const SDL_FRect *dst);

    // Draws text, truncating with "..." past maxW. Returns drawn width.
    int text(int x, int y, const std::string &s, FontSize fs, SDL_Color c, int maxW = 0);
    int textRight(int right, int y, const std::string &s, FontSize fs, SDL_Color c);
    int measure(const std::string &s, FontSize fs);
    int lineHeight(FontSize fs) const { return lineH[fs]; }
    // Window pixels per layout pixel; above 1 when a desktop window is bigger than 1280 x 720.
    float pixelScale() const { return scale; }
    // Splits text into lines no wider than w, keeping explicit line breaks.
    std::vector<std::string> wrap(const std::string &s, FontSize fs, int w);

    // Drops cached textures not used for a while.
    void endFrame();

private:
    struct Cached {
        SDL_Texture *tex;
        float w, h;  // layout pixels
        unsigned lastUsed;
    };
    Cached *get(const std::string &s, FontSize fs, SDL_Color c);
    std::string fit(const std::string &s, FontSize fs, int maxW);
    void updateScale();

    std::vector<SDL_Texture *> icons;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    TTF_Font *fonts[FS_COUNT] = {};      // layout-size fonts, used for measuring
    TTF_Font *drawFonts[FS_COUNT] = {};  // fonts at the window scale, when it is not 1
    const void *fontData = nullptr;
    int fontSize = 0;
    float scale = 1;
    int lineH[FS_COUNT] = {};
    std::unordered_map<std::string, Cached> cache;
    unsigned frame = 0;
};
