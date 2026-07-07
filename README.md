# Spotiamp
![UI](https://raw.githubusercontent.com/kran27/Spotiamp/main/uidemo.png)
Features:
- New Modern Theme
- Fixed Album Art Support
- Improved Theming Support (WIP)


# What is it?

Spotiamp is a now-abandoned tribute to Winamp made by a Spotify developer. I like the compactness and customizability of Winamp, and I am a Spotify user, so it made sense to use it, and update it a bit to my liking.

## How was it made?
Originally I was modifying the original executable, but there's only so far that can get you. unfortunately, despite the program being open source, it used an older type of API authentication which meant that obtaining a new key was completely impossible. that being the only obstacle between me and compiling the source code, I reverse engineered the original executable to obtain the key, and put it into the source code.

## Build

Install CMake and Conan 2 first. On macOS:

```sh
brew install cmake conan
conan profile detect --force
```

Set Spotify application credentials before configuring a fresh build directory:

```sh
export SPOTIFY_CLIENT_ID=your_client_id
export SPOTIFY_CLIENT_SECRET=your_client_secret
```

Install dependencies and build:

```sh
conan install . -of build/conan -s build_type=Release --build=missing
cmake -B build/conan-cmake \
  -DCMAKE_TOOLCHAIN_FILE=build/conan/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/conan-cmake
```

The first Conan install can take a while because it may build SDL2, libcurl, OpenSSL, and zlib from source. Later builds should reuse the Conan cache.

### Windows from macOS

Install a MinGW-w64 toolchain:

```sh
brew install mingw-w64
```

Then use the Windows host profile:

```sh
conan install . -of build/conan-win \
  -pr:b=default \
  -pr:h=conan/profiles/windows-mingw-x64 \
  --build=missing
cmake -B build/windows \
  -DCMAKE_TOOLCHAIN_FILE=build/conan-win/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/windows
```

- Playlist screen uses [Montserrat Medium](https://fonts.google.com/download?family=Montserrat)
