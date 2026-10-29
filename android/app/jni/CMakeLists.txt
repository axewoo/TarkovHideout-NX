# Native part of the Android app: SDL2, SDL2_ttf and SDL2_image built from source (see
# fetch-deps.sh) plus the same sources as the Switch and desktop builds, as libmain.so.
cmake_minimum_required(VERSION 3.18)
project(TarkovHideout C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(DEPS ${CMAKE_CURRENT_SOURCE_DIR}/../../deps)
file(STRINGS ${ROOT}/Makefile version_line REGEX "^APP_VERSION")
string(REGEX REPLACE ".*:= *" "" APP_VERSION "${version_line}")

set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/SDL2 SDL2)

set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/SDL2_ttf SDL2_ttf)

# stb_image covers the JPEG and PNG files the app loads; nothing else is needed.
foreach(fmt AVIF BMP GIF JXL LBM PCX PNM QOI SVG TGA TIF WEBP XCF XPM XV)
    set(SDL2IMAGE_${fmt} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDL2IMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${DEPS}/SDL2_image SDL2_image)

# The sources include <SDL2/SDL_ttf.h> and <SDL2/SDL_image.h>, as installed SDL packages lay them out.
configure_file(${DEPS}/SDL2_ttf/SDL_ttf.h ${CMAKE_CURRENT_BINARY_DIR}/inc/SDL2/SDL_ttf.h COPYONLY)
configure_file(${DEPS}/SDL2_image/include/SDL_image.h ${CMAKE_CURRENT_BINARY_DIR}/inc/SDL2/SDL_image.h COPYONLY)

add_library(main SHARED
    ${ROOT}/source/main.cpp
    ${ROOT}/source/model.cpp
    ${ROOT}/source/net.cpp
    ${ROOT}/source/ui.cpp
    ${ROOT}/desktop/nxshim.cpp)
set_target_properties(main PROPERTIES CXX_STANDARD 17 CXX_EXTENSIONS ON)
target_include_directories(main PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/inc ${ROOT}/include ${ROOT}/source ${ROOT}/desktop)
target_compile_definitions(main PRIVATE APP_VERSION="${APP_VERSION}")
target_link_libraries(main PRIVATE SDL2::SDL2 SDL2_ttf::SDL2_ttf SDL2_image::SDL2_image)
if(ANDROID)
    target_link_libraries(main PRIVATE android log)
endif()
