cmake_minimum_required(VERSION 3.16)
project(crossfade VERSION 0.1.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

if(WIN32)
    # No glib / libnotify on Windows: the Linux desktop-integration code is
    # compiled out (see win32/ stubs). libmpv is discovered automatically via
    # pkg-config (MSYS2) or find_library/find_path; an explicit
    # -DMPV_LIBRARY=... / -DMPV_INCLUDE_DIR=... still takes precedence.
    set(P100_WIN TRUE)
    add_compile_definitions(P100_WINDOWS)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MPV QUIET mpv)
    endif()
    find_library(MPV_LIBRARY NAMES mpv libmpv HINTS ${MPV_LIBRARY_DIRS})
    find_path(MPV_INCLUDE_DIR mpv/client.h HINTS ${MPV_INCLUDE_DIRS})
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED mpv)
    pkg_check_modules(NOTIFY REQUIRED libnotify)
    pkg_check_modules(GLIB REQUIRED glib-2.0 gobject-2.0 gio-2.0)
endif()

# ---- ImGui ----
add_library(imgui STATIC
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_sdl2.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
    external/imgui/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(imgui PUBLIC external/imgui external/imgui/backends)
target_link_libraries(imgui PUBLIC SDL2::SDL2 OpenGL::GL)

# ---- crossfade sources ----
set(SOURCES
    main.cpp
    config.cpp
    accent.cpp
    paths.cpp
    ytdlp.cpp
    mpvplayer.cpp
    ui_helpers.cpp
    ui_theme.cpp
    ui_backdrop.cpp
    ui_layout.cpp
    ui_components.cpp
    ui_icons.cpp
    ui_top.cpp
    ui_main.cpp
    ui_playlists.cpp
    ui_youtube.cpp
    ui_album.cpp
    ui_artists.cpp
    ui_bottom.cpp
    ui_queue.cpp
    ui_radio.cpp
    ui_lyrics.cpp
    ui_notify.cpp
    ui_settings.cpp
    ui_stats.cpp
    ui_party.cpp
    texture.cpp
    writeback.cpp
    party.cpp
    party_link.cpp
    party_queue.cpp
    party_util.cpp
    party_net.cpp
    party_server.cpp
    external/qrcodegen.cpp
)

if(P100_WIN)
    # Linux desktop integration has no Windows counterpart yet; the stubs
    # keep the same class/function surface so main.cpp compiles untouched.
    list(APPEND SOURCES win32/mpris_stub.cpp win32/discord_rpc_stub.cpp)
else()
    list(APPEND SOURCES mpris.cpp discord_rpc.cpp)
endif()

add_executable(crossfade ${SOURCES})

# Bake in the source directory so the Python backend / venv can still be
# found when the binary is copied elsewhere (e.g. /bin) or run from build/.
target_compile_definitions(crossfade PRIVATE
    P100_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

if(P100_WIN)
    target_include_directories(crossfade PRIVATE ${MPV_INCLUDE_DIR} external)
    target_link_libraries(crossfade
        imgui
        SDL2::SDL2main
        ${MPV_LIBRARY}
        Threads::Threads
        winmm
        ws2_32
        iphlpapi
        bcrypt
    )
else()
    target_include_directories(crossfade PRIVATE
        ${MPV_INCLUDE_DIRS}
        ${NOTIFY_INCLUDE_DIRS}
        ${GLIB_INCLUDE_DIRS}
        external
    )
    target_link_libraries(crossfade
        imgui
        ${MPV_LIBRARIES}
        ${NOTIFY_LIBRARIES}
        ${GLIB_LIBRARIES}
        Threads::Threads
    )
endif()

# ---- Install ----
include(GNUInstallDirs)

install(TARGETS crossfade RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    p100-backend.py
    p100-backend-oauth.py
    requirements.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/crossfade
)
install(DIRECTORY assets/fonts DESTINATION ${CMAKE_INSTALL_DATADIR}/crossfade/assets)
install(PROGRAMS scripts/crossfade-login DESTINATION ${CMAKE_INSTALL_BINDIR})

if(UNIX)
    install(FILES packaging/crossfade.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/crossfade.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
