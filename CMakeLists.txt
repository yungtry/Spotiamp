cmake_minimum_required(VERSION 3.5)
project(spotiamp)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# ---------------------------------------------------------------------------
# Credentials — generated at configure time from environment variables.
# Never stored in source control. Set before running cmake:
#   export SPOTIFY_CLIENT_ID=...
#   export SPOTIFY_CLIENT_SECRET=...
#   cmake -B build && cmake --build build
# ---------------------------------------------------------------------------
set(CREDENTIALS_DIR "${CMAKE_BINARY_DIR}/generated")
set(CREDENTIALS_H   "${CREDENTIALS_DIR}/credentials.h")

# Read env vars at configure time
set(SPOTIAMP_CLIENT_ID     "$ENV{SPOTIFY_CLIENT_ID}")
set(SPOTIAMP_CLIENT_SECRET "$ENV{SPOTIFY_CLIENT_SECRET}")

if("${SPOTIAMP_CLIENT_ID}" STREQUAL "" OR "${SPOTIAMP_CLIENT_SECRET}" STREQUAL "")
  if(EXISTS "${CREDENTIALS_H}")
    message(STATUS "Using existing generated Spotify credentials header")
  else()
    message(FATAL_ERROR
      "\nSPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET must be set before running cmake.\n"
      "Register an app at https://developer.spotify.com/dashboard\n"
      "Set redirect URI to http://127.0.0.1:3000/callback, then:\n"
      "  export SPOTIFY_CLIENT_ID=your_client_id\n"
      "  export SPOTIFY_CLIENT_SECRET=your_client_secret\n"
      "  cmake -B build && cmake --build build\n"
    )
  endif()
else()
  # Run the Python script now, at configure time, passing credentials as args
  # so they never need to be in the environment at build time.
  execute_process(
    COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/gen_credentials.py"
            "${CREDENTIALS_DIR}"
            "${SPOTIAMP_CLIENT_ID}"
            "${SPOTIAMP_CLIENT_SECRET}"
    RESULT_VARIABLE CRED_RESULT
    OUTPUT_VARIABLE CRED_OUTPUT
    ERROR_VARIABLE  CRED_ERROR
  )
  if(NOT CRED_RESULT EQUAL 0)
    message(FATAL_ERROR "gen_credentials.py failed:\n${CRED_ERROR}")
  endif()
  message(STATUS "${CRED_OUTPUT}")
endif()

include_directories("${CREDENTIALS_DIR}")

set(SPOTIAMP_SRCS
    commands.h
    dialogs.h
    resource.h
    spotifyamp.cpp
    spotifyamp.h
    stdafx.h
    targetver.h
    window.cpp
    window.h
    window_sdl.cpp
    window_sdl.h
    visplugin.h
    zipfile.cpp
    zipfile.h
    audio_eq.c
    audio_eq.h
    fpu_util.h
    tinfl.c
    tinfl.h
    shoutcast.h
    mp3.h
    build_config.h
    config.h
    types.h
    util.h
    appkey.h
    tiny_spotify/tiny_spotify.h
)

# Cross-platform source files selection
if(WIN32 AND NOT FORCE_SDL)
    list(APPEND SPOTIAMP_SRCS
        dialogs_win32.cpp
        window_win32.cpp
        window_win32.h
        audio_win32.c
        vis.cpp
        visplugin.cpp
        com_ptr.h
        tiny_spotify/tiny_spotify.obj
        spotifyamp.rc
        app.manifest
    )
else()
    # macOS, Linux, or forced SDL
    if(APPLE)
        list(APPEND SPOTIAMP_SRCS
            dialogs_mac.mm
            audio_sdl.cpp
            vis_sdl.cpp
            tiny_spotify/tiny_spotify_mock.cpp
        )
        find_library(COCOA_LIBRARY Cocoa)
        list(APPEND SPOTIAMP_LIBS ${COCOA_LIBRARY})
    else()
        list(APPEND SPOTIAMP_SRCS
            dialogs_sdl.cpp
            audio_sdl.cpp
            vis_sdl.cpp
            tiny_spotify/tiny_spotify_mock.cpp
        )
    endif()
    add_definitions(-DWITH_SDL)
endif()

# Include directories
include_directories(tiny_spotify)
include_directories(${SDL2_INCLUDE_DIRS})
link_directories(${SDL2_LIBRARY_DIRS})

# Compile flags
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /D_CRT_NONSTDC_NO_WARNINGS")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O1 /Ob2 /Oi /Os /Oy /GF /Gy /fp:fast")
    string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    string(REGEX REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_UNICODE -DUNICODE)
else()
    # gcc or clang (macOS / Linux / MinGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -fno-exceptions -fno-rtti")
endif()

add_executable(spotiamp ${SPOTIAMP_SRCS})

add_custom_command(TARGET spotiamp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Skins"
            "$<TARGET_FILE_DIR:spotiamp>/Skins"
)

# Linking
if(MSVC)
    set_property(TARGET spotiamp APPEND PROPERTY LINK_FLAGS "/NXCOMPAT:NO /SAFESEH:NO")
endif()

find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIRS})

target_link_libraries(spotiamp ${SDL2_LIBRARIES} ${SPOTIAMP_LIBS} ${CURL_LIBRARIES})
