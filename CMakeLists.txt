cmake_minimum_required(VERSION 3.16)
project(crossfade VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single source of truth for the C++ version string. CMakeLists.txt's project()
# version is checked against packaging metadata in CI.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)

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
    src/main.cpp
    src/app/app.cpp
    src/core/power.cpp
    src/core/tasks.cpp
    src/data/accent.cpp
    src/data/paths.cpp
    src/net/ytdlp.cpp
    src/playback/crossfade.cpp
    src/playback/session.cpp
    src/playback/mpvplayer.cpp
    src/ui/ui_helpers.cpp
    src/ui/format.cpp
    src/ui/playlists_io.cpp
    src/ui/ui_theme.cpp
    src/ui/ui_backdrop.cpp
    src/ui/ui_layout.cpp
    src/ui/ui_components.cpp
    src/ui/ui_icons.cpp
    src/ui/ui_top.cpp
    src/ui/ui_main.cpp
    src/ui/ui_playlists.cpp
    src/ui/ui_youtube.cpp
    src/ui/ui_album.cpp
    src/ui/ui_artists.cpp
    src/ui/ui_bottom.cpp
    src/ui/ui_queue.cpp
    src/ui/ui_radio.cpp
    src/ui/ui_lyrics.cpp
    src/ui/ui_notify.cpp
    src/ui/ui_settings.cpp
    src/ui/ui_stats.cpp
    src/ui/ui_party.cpp
    src/ui/ui_podcasts.cpp
    src/net/texture.cpp
    src/data/writeback.cpp
    src/party/party.cpp
    src/party/party_link.cpp
    src/party/party_queue.cpp
    src/party/party_util.cpp
    src/party/party_net.cpp
    src/party/party_server.cpp
    src/net/updater.cpp
    src/data/library.cpp
    src/data/store.cpp
    src/data/cache.cpp
    src/app/diagnostics.cpp
    external/qrcodegen.cpp
    external/sqlite/sqlite3.c
)

if(P100_WIN)
    # Linux desktop integration has no Windows counterpart yet; the stubs
    # keep the same class/function surface so main.cpp compiles untouched.
    list(APPEND SOURCES win32/mpris_stub.cpp win32/discord_rpc_stub.cpp)
else()
    list(APPEND SOURCES src/playback/mpris.cpp src/net/discord_rpc.cpp)
endif()

add_executable(crossfade ${SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CROSSFADE_WARNINGS -Wall -Wextra -Wno-unused-parameter)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND CROSSFADE_WARNINGS -Wno-ignored-attributes)
    endif()
    target_compile_options(crossfade PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${CROSSFADE_WARNINGS}>)
endif()

# Bake in the source directory so the Python backend / venv can still be
# found when the binary is copied elsewhere (e.g. /bin) or run from build/.
target_compile_definitions(crossfade PRIVATE
    P100_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SQLITE_ENABLE_FTS5=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_THREADSAFE=1
)

target_include_directories(crossfade PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/party
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/playback
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
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
        shell32
        dbghelp
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
install(DIRECTORY py/cf_backend
    DESTINATION ${CMAKE_INSTALL_DATADIR}/crossfade/py
)
install(DIRECTORY assets/fonts DESTINATION ${CMAKE_INSTALL_DATADIR}/crossfade/assets)
install(PROGRAMS scripts/crossfade-login DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/crossfade-login-helper.py
    DESTINATION ${CMAKE_INSTALL_DATADIR}/crossfade/scripts)

# ---- Party mode unit tests (off by default; ./mk stays unchanged) ----
option(CROSSFADE_BUILD_TESTS "Build crossfade unit tests" ON)
if(CROSSFADE_BUILD_TESTS)
    enable_testing()
    foreach(party_test party_link party_queue party_util)
        add_executable(test_${party_test} tests/test_${party_test}.cpp
            src/party/${party_test}.cpp)
        target_include_directories(test_${party_test} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/party
            ${CMAKE_CURRENT_SOURCE_DIR}/src/net)
        target_link_libraries(test_${party_test} Threads::Threads)
        add_test(NAME ${party_test} COMMAND test_${party_test})
    endforeach()
    if(WIN32)
        target_link_libraries(test_party_util bcrypt)
    endif()

    add_executable(test_crossfade tests/test_crossfade.cpp
        src/playback/crossfade.cpp)
    target_include_directories(test_crossfade PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/playback
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
    target_link_libraries(test_crossfade Threads::Threads)
    add_test(NAME crossfade COMMAND test_crossfade)

    add_executable(test_tasks tests/test_tasks.cpp src/core/tasks.cpp)
    target_include_directories(test_tasks PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
    target_link_libraries(test_tasks Threads::Threads)
    add_test(NAME tasks COMMAND test_tasks)

    add_executable(test_library tests/test_library.cpp
        src/data/library.cpp src/net/ytdlp.cpp src/data/paths.cpp
        external/sqlite/sqlite3.c)
    target_include_directories(test_library PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/data
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/src/playback
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app
        ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/external)
    target_compile_definitions(test_library PRIVATE
        P100_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        SQLITE_ENABLE_FTS5=1
        SQLITE_OMIT_LOAD_EXTENSION=1
        SQLITE_THREADSAFE=1)
    target_link_libraries(test_library Threads::Threads)
    add_test(NAME library COMMAND test_library)

    add_executable(test_cache tests/test_cache.cpp
        src/net/ytdlp.cpp src/data/paths.cpp src/data/library.cpp
        external/sqlite/sqlite3.c)
    target_include_directories(test_cache PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/data
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
    target_compile_definitions(test_cache PRIVATE
        SQLITE_ENABLE_FTS5=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1)
    target_link_libraries(test_cache Threads::Threads)
    add_test(NAME cache COMMAND test_cache)

    add_executable(test_json tests/test_json.cpp
        src/net/ytdlp.cpp src/data/paths.cpp src/data/library.cpp
        external/sqlite/sqlite3.c)
    target_include_directories(test_json PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/data
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
    target_compile_definitions(test_json PRIVATE
        SQLITE_ENABLE_FTS5=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1)
    target_link_libraries(test_json Threads::Threads)
    add_test(NAME json COMMAND test_json)

    add_executable(test_session tests/test_session.cpp
        src/playback/session.cpp src/data/cache.cpp src/ui/playlists_io.cpp
        src/core/tasks.cpp src/net/ytdlp.cpp src/data/paths.cpp
        src/data/library.cpp external/sqlite/sqlite3.c)
    target_include_directories(test_session PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/data
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/src/playback
        ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
        ${MPV_INCLUDE_DIRS}
        ${MPV_INCLUDE_DIR})
    target_compile_definitions(test_session PRIVATE
        SQLITE_ENABLE_FTS5=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1)
    target_link_libraries(test_session Threads::Threads)
    add_test(NAME session COMMAND test_session)

    add_executable(test_playlists tests/test_playlists.cpp
        src/ui/playlists_io.cpp src/core/tasks.cpp src/net/ytdlp.cpp
        src/data/paths.cpp src/data/library.cpp external/sqlite/sqlite3.c)
    target_include_directories(test_playlists PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/data
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
    target_compile_definitions(test_playlists PRIVATE
        SQLITE_ENABLE_FTS5=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=1)
    target_link_libraries(test_playlists Threads::Threads)
    add_test(NAME playlists COMMAND test_playlists)

    add_executable(test_settings tests/test_settings.cpp)
    target_include_directories(test_settings PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui)
    add_test(NAME settings COMMAND test_settings)

    # Headless HTTP harness used by scripts/party-smoke-test.sh.
    add_executable(party_smoke_server
        tests/party_smoke_server.cpp
        src/party/party_server.cpp
        src/party/party_util.cpp
    )
    target_include_directories(party_smoke_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/party
        ${CMAKE_CURRENT_SOURCE_DIR}/src/net
        external)
    target_link_libraries(party_smoke_server Threads::Threads)
    if(WIN32)
        target_link_libraries(party_smoke_server ws2_32 iphlpapi bcrypt)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        foreach(tgt test_party_link test_party_queue test_party_util test_library
                    test_crossfade test_cache test_json test_session test_playlists
                    test_settings)
            target_compile_options(${tgt} PRIVATE ${CROSSFADE_WARNINGS})
        endforeach()
    endif()
endif()

if(UNIX)
    install(FILES packaging/crossfade.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/crossfade.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
