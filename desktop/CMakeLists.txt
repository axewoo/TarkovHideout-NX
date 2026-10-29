# Desktop build (Windows and Linux) of the same app the Switch Makefile builds.
#   cmake -S desktop -B build-desktop -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-desktop -j
cmake_minimum_required(VERSION 3.16)
project(TarkovHideout CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# One version number for both builds: the Switch Makefile's.
file(STRINGS ${ROOT}/Makefile version_line REGEX "^APP_VERSION")
string(REGEX REPLACE ".*:= *" "" APP_VERSION "${version_line}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET sdl2 SDL2_ttf SDL2_image libcurl)

set(SOURCES
    ${ROOT}/source/main.cpp
    ${ROOT}/source/model.cpp
    ${ROOT}/source/net.cpp
    ${ROOT}/source/ui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/nxshim.cpp)
if(WIN32)
    list(APPEND SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/app.rc)
endif()

add_executable(TarkovHideout WIN32 ${SOURCES})
target_include_directories(TarkovHideout PRIVATE ${ROOT}/include ${ROOT}/source ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(TarkovHideout PRIVATE APP_VERSION="${APP_VERSION}")
target_compile_options(TarkovHideout PRIVATE -Wall)
target_link_libraries(TarkovHideout PRIVATE PkgConfig::DEPS)
if(WIN32)
    target_compile_definitions(TarkovHideout PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)
    target_link_options(TarkovHideout PRIVATE -static-libgcc -static-libstdc++)
endif()

# The font and icon sit next to the program, like in the release packages.
add_custom_command(TARGET TarkovHideout POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/font.otf
            ${CMAKE_CURRENT_SOURCE_DIR}/icon.png $<TARGET_FILE_DIR:TarkovHideout>)
