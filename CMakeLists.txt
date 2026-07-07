cmake_minimum_required(VERSION 3.16)
project(spotiamp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

find_package(CURL REQUIRED)

find_package(SDL2 CONFIG QUIET)
if(TARGET SDL2::SDL2)
  set(SPOTIAMP_SDL_LIBS SDL2::SDL2)
  if(TARGET SDL2::SDL2main)
    list(PREPEND SPOTIAMP_SDL_LIBS SDL2::SDL2main)
  endif()
elseif(TARGET SDL2::SDL2-static)
  set(SPOTIAMP_SDL_LIBS SDL2::SDL2-static)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  set(SPOTIAMP_SDL_LIBS ${SDL2_LIBRARIES})
endif()

if(TARGET CURL::libcurl)
  set(SPOTIAMP_CURL_LIBS CURL::libcurl)
else()
  set(SPOTIAMP_CURL_LIBS ${CURL_LIBRARIES})
endif()

set(SPOTIAMP_SRCS
    ${SRC_DIR}/commands.h
    ${SRC_DIR}/dialogs.h
    ${SRC_DIR}/spotifyamp.cpp
    ${SRC_DIR}/spotifyamp.h
    ${SRC_DIR}/stdafx.h
    ${SRC_DIR}/window.cpp
    ${SRC_DIR}/window.h
    ${SRC_DIR}/window_sdl.cpp
    ${SRC_DIR}/window_sdl.h
    ${SRC_DIR}/zipfile.cpp
    ${SRC_DIR}/zipfile.h
    ${SRC_DIR}/audio_eq.c
    ${SRC_DIR}/audio_eq.h
    ${SRC_DIR}/fpu_util.h
    ${SRC_DIR}/tinfl.c
    ${SRC_DIR}/tinfl.h
    ${SRC_DIR}/shoutcast.h
    ${SRC_DIR}/build_config.h
    ${SRC_DIR}/config.h
    ${SRC_DIR}/types.h
    ${SRC_DIR}/tiny_spotify/tiny_spotify.h
    ${SRC_DIR}/tiny_spotify/tiny_spotify_mock.cpp
    ${SRC_DIR}/audio_sdl.cpp
    ${SRC_DIR}/vis_sdl.cpp
)

if(APPLE)
    enable_language(OBJCXX)
    list(APPEND SPOTIAMP_SRCS ${SRC_DIR}/dialogs_mac.mm)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    list(APPEND SPOTIAMP_PLATFORM_LIBS ${COCOA_LIBRARY})
else()
    list(APPEND SPOTIAMP_SRCS ${SRC_DIR}/dialogs_sdl.cpp)
endif()

if(WIN32)
    enable_language(RC)
    list(APPEND SPOTIAMP_SRCS
        ${SRC_DIR}/windows_resources.rc
        ${SRC_DIR}/icon.ico
    )
    list(APPEND SPOTIAMP_PLATFORM_LIBS ws2_32)
endif()

add_executable(spotiamp ${SPOTIAMP_SRCS})

target_compile_definitions(spotiamp PRIVATE WITH_SDL)

target_include_directories(spotiamp PRIVATE
    "${CREDENTIALS_DIR}"
    "${SRC_DIR}"
    "${SRC_DIR}/tiny_spotify"
    ${SDL2_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

if(SDL2_LIBRARY_DIRS)
  target_link_directories(spotiamp PRIVATE ${SDL2_LIBRARY_DIRS})
endif()

if(MSVC)
    target_compile_definitions(spotiamp PRIVATE _CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
    target_compile_options(spotiamp PRIVATE /GR-)
else()
    target_compile_options(spotiamp PRIVATE -O3 -fno-exceptions -fno-rtti)
endif()

add_custom_command(TARGET spotiamp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Skins"
            "$<TARGET_FILE_DIR:spotiamp>/Skins"
)

# Linking
if(MSVC)
    set_property(TARGET spotiamp APPEND PROPERTY LINK_FLAGS "/NXCOMPAT:NO /SAFESEH:NO")
endif()

target_link_libraries(spotiamp PRIVATE ${SPOTIAMP_SDL_LIBS} ${SPOTIAMP_PLATFORM_LIBS} ${SPOTIAMP_CURL_LIBS})
