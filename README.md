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

<details>
<summary><strong>Expand to see build instructions</strong></summary>

Install CMake and Conan 2 first. On macOS:

```sh
brew install cmake conan
conan profile detect --force
```

Before configuring a fresh build directory, you need to provide your Spotify application credentials. The CMake build requires these to link authentication keys into the binary.

1. Go to the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard) and log in.
2. Click **Create app** and set up a new application.
3. In the application settings, set the **Redirect URI** exactly to: `http://127.0.0.1:3000/callback`
4. Once created, copy the **Client ID** and **Client Secret**, and export them in your terminal:

```sh
export SPOTIFY_CLIENT_ID=your_client_id
export SPOTIFY_CLIENT_SECRET=your_client_secret
```

Install dependencies:

```sh
conan install . -of build/conan -s build_type=Release --build=missing
```

The first Conan install can take a while because it may build SDL2, libcurl, OpenSSL, and zlib from source. Later builds should reuse the Conan cache.
OpenSSL is built without command-line apps, FIPS, and legacy providers; Spotiamp only needs libcurl TLS support, and disabling those extras avoids MinGW cross-linking failures.

### Rust playback bridge

Spotiamp uses the Rust playback bridge for audio playback. Cargo is required when configuring the CMake build:

```sh
cargo test --manifest-path playback/librespot_bridge/Cargo.toml
cmake -B build/conan-cmake \
  -DCMAKE_TOOLCHAIN_FILE=build/conan/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/conan-cmake
```

For cross-compilation, pass the matching Cargo target triple:

```sh
cmake -B build/windows \
  -DCMAKE_TOOLCHAIN_FILE=build/conan-win/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DSPOTIAMP_CARGO_TARGET=x86_64-pc-windows-gnu
```

### Windows from macOS

Install a MinGW-w64 toolchain:

```sh
brew install mingw-w64
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
rustup target add x86_64-pc-windows-gnu
```

Then use the Windows host profile:

```sh
conan install . -of build/conan-win \
  -pr:b=default \
  -pr:h=conan/profiles/windows-mingw-x64 \
  --build=missing
cmake -B build/windows \
  -DCMAKE_TOOLCHAIN_FILE=build/conan-win/build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DSPOTIAMP_CARGO_TARGET=x86_64-pc-windows-gnu
cmake --build build/windows
```

</details>

- Playlist screen uses [Montserrat Medium](https://fonts.google.com/download?family=Montserrat)
