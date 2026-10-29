#!/bin/sh
# Downloads the SDL sources the Android build compiles (SDL2, SDL2_ttf with its vendored
# FreeType and HarfBuzz, SDL2_image with stb_image) into android/deps.
set -e
cd "$(dirname "$0")"
mkdir -p deps
get() {  # name repo version
    [ -d "deps/$1" ] && return
    curl -sSfL "https://github.com/libsdl-org/$2/releases/download/release-$3/$1-$3.tar.gz" | tar -xz -C deps
    mv "deps/$1-$3" "deps/$1"
}
get SDL2 SDL 2.30.9
get SDL2_ttf SDL_ttf 2.22.0
get SDL2_image SDL_image 2.8.2
