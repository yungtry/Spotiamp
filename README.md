# Spotiamp

[![Build](https://github.com/yungtry/Spotiamp/actions/workflows/build.yml/badge.svg)](https://github.com/yungtry/Spotiamp/actions/workflows/build.yml)

![UI](https://raw.githubusercontent.com/kran27/Spotiamp/main/uidemo.png)

Features:
- New Modern Theme
- Fixed Album Art Support
- Improved Theming Support (WIP)

<details>
<summary>TODO / Roadmap</summary>

Implemented:

- [x] Librespot player backend
- [x] Modern Spotify API usage
- [x] Working likes
- [x] Discover Weekly

To be implemented:

- [ ] Fix Album Art support
- [ ] Liking songs
- [ ] Adding songs to playlists
- [ ] Removing songs from playlists
- [ ] Creating playlists
- [ ] Radios
- [ ] Milkdrop visualizer

</details>


# What is it?

Spotiamp is a now-abandoned tribute to Winamp made by a Spotify developer. I like the compactness and customizability of Winamp, and I am a Spotify user, so it made sense to use it, and update it a bit to my liking.

## How was it made?

This project updates the original open-source Spotiamp interface with modern Spotify OAuth, cross-platform builds, and a librespot-based playback bridge.

## Build

### 1. Create a Spotify app

1. Open the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard).
2. Click **Create app**.
3. Add this Redirect URI: `http://127.0.0.1:3000/callback`
4. Save the app and copy its **Client ID**.

You do not need the Client Secret.

### 2. Download the code

```sh
git clone https://github.com/yungtry/Spotiamp.git
cd Spotiamp
```

### macOS

Install [Homebrew](https://brew.sh/) first. Then run:

```sh
brew install cmake conan rust
conan profile detect --force
export SPOTIFY_CLIENT_ID="paste_your_client_id_here"
conan install . -of build/conan -s build_type=Release --build=missing
cmake -S . -B build/app -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/conan/build/Release/generators/conan_toolchain.cmake
cmake --build build/app --parallel
```

The finished program is `build/app/spotiamp`.

### Windows

Install these first:

- [Visual Studio 2022 Community](https://visualstudio.microsoft.com/vs/community/) with **Desktop development with C++** selected
- [Python](https://www.python.org/downloads/)
- [CMake](https://cmake.org/download/)
- [Rust](https://rustup.rs/)
- [Git](https://git-scm.com/download/win)

Open **Developer PowerShell for VS 2022**, enter the Spotiamp folder, and run:

```powershell
py -m pip install conan
conan profile detect --force
$env:SPOTIFY_CLIENT_ID="paste_your_client_id_here"
conan install . -of build/conan -s build_type=Release --build=missing
cmake -S . -B build/app -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/conan/build/Release/generators/conan_toolchain.cmake
cmake --build build/app --config Release --parallel
```

The finished program is `build/app/Release/spotiamp.exe`.

### Build Windows version on macOS

Run this from the Spotiamp folder:

```sh
brew install cmake conan rust mingw-w64
conan profile detect --force
rustup target add x86_64-pc-windows-gnu
export SPOTIFY_CLIENT_ID="paste_your_client_id_here"
conan install . -of build/conan-win -pr:b=default -pr:h=conan/profiles/windows-mingw-x64 --build=missing
cmake -S . -B build/windows -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/conan-win/build/Release/generators/conan_toolchain.cmake -DSPOTIAMP_CARGO_TARGET=x86_64-pc-windows-gnu
cmake --build build/windows --parallel
```

The finished Windows program is `build/windows/spotiamp.exe`.

### GitHub automatic builds

Add `SPOTIFY_CLIENT_ID` in **Settings → Secrets and variables → Actions → New repository secret**. Open the **Actions** tab and run the **Build** workflow. Download `spotiamp-windows` or `spotiamp-macos` from the finished run.

- Playlist screen uses [Montserrat Medium](https://fonts.google.com/download?family=Montserrat)
