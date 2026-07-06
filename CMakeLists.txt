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
set(SRC_DIR         "${CMAKE_SOURCE_DIR}/src")

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
include_directories("${SRC_DIR}")
include_directories("${SRC_DIR}/tiny_spotify")
include_directories("${SRC_DIR}/winamp_sdk")

set(SPOTIAMP_SRCS
    ${SRC_DIR}/commands.h
    ${SRC_DIR}/dialogs.h
    ${SRC_DIR}/resource.h
    ${SRC_DIR}/spotifyamp.cpp
    ${SRC_DIR}/spotifyamp.h
    ${SRC_DIR}/stdafx.h
    ${SRC_DIR}/targetver.h
    ${SRC_DIR}/window.cpp
    ${SRC_DIR}/window.h
    ${SRC_DIR}/window_sdl.cpp
    ${SRC_DIR}/window_sdl.h
    ${SRC_DIR}/visplugin.h
    ${SRC_DIR}/zipfile.cpp
    ${SRC_DIR}/zipfile.h
    ${SRC_DIR}/audio_eq.c
    ${SRC_DIR}/audio_eq.h
    ${SRC_DIR}/fpu_util.h
    ${SRC_DIR}/tinfl.c
    ${SRC_DIR}/tinfl.h
    ${SRC_DIR}/shoutcast.h
    ${SRC_DIR}/mp3.h
    ${SRC_DIR}/build_config.h
    ${SRC_DIR}/config.h
    ${SRC_DIR}/types.h
    ${SRC_DIR}/util.h
    ${SRC_DIR}/appkey.h
    ${SRC_DIR}/tiny_spotify/tiny_spotify.h
)

# Cross-platform source files selection
if(WIN32 AND NOT FORCE_SDL)
    list(APPEND SPOTIAMP_SRCS
        ${SRC_DIR}/dialogs_win32.cpp
        ${SRC_DIR}/window_win32.cpp
        ${SRC_DIR}/window_win32.h
        ${SRC_DIR}/audio_win32.c
        ${SRC_DIR}/vis.cpp
        ${SRC_DIR}/visplugin.cpp
        ${SRC_DIR}/com_ptr.h
        ${SRC_DIR}/tiny_spotify/tiny_spotify.obj
        ${SRC_DIR}/spotifyamp.rc
        ${SRC_DIR}/app.manifest
    )
else()
    # macOS, Linux, or forced SDL
    if(APPLE)
        list(APPEND SPOTIAMP_SRCS
            ${SRC_DIR}/dialogs_mac.mm
            ${SRC_DIR}/audio_sdl.cpp
            ${SRC_DIR}/vis_sdl.cpp
            ${SRC_DIR}/tiny_spotify/tiny_spotify_mock.cpp
        )
        find_library(COCOA_LIBRARY Cocoa)
        list(APPEND SPOTIAMP_LIBS ${COCOA_LIBRARY})
    else()
        list(APPEND SPOTIAMP_SRCS
            ${SRC_DIR}/dialogs_sdl.cpp
            ${SRC_DIR}/audio_sdl.cpp
            ${SRC_DIR}/vis_sdl.cpp
            ${SRC_DIR}/tiny_spotify/tiny_spotify_mock.cpp
        )
    endif()
    add_definitions(-DWITH_SDL)
endif()

# Include directories
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
