#!/usr/bin/env python3
"""
gen_credentials.py  —  run at build time by CMake.

Writes the public Spotify client ID to credentials.h in the build directory.

The obfuscation is NOT cryptographic — a determined person can still reverse
it. The point is:
  1. The plaintext strings are not trivially grep-able in the binary.
  2. Generated build configuration stays out of source control.
  3. Each developer uses their own Spotify app client ID.

Usage (called automatically by CMake):
  python3 scripts/gen_credentials.py <output_dir>
"""

import os
import sys

XOR_KEY = 0x5A  # single-byte XOR key, embedded in the source — not a secret


def obfuscate(s: str) -> list[int]:
    return [b ^ XOR_KEY for b in s.encode("utf-8")]


def fmt_bytes(data: list[int]) -> str:
    return ", ".join(f"0x{b:02X}" for b in data)


def main():
    if len(sys.argv) < 3:
        print("Usage: gen_credentials.py <output_dir> <client_id>", file=sys.stderr)
        sys.exit(1)

    output_dir    = sys.argv[1]
    client_id     = sys.argv[2]
    if not client_id:
        print(
            "\n"
            "  ERROR: SPOTIFY_CLIENT_ID must be set before running cmake.\n"
            "\n"
            "  Register a free app at https://developer.spotify.com/dashboard,\n"
            "  then set the redirect URI to  http://127.0.0.1:3000/callback\n"
            "\n"
            "  export SPOTIFY_CLIENT_ID=your_client_id\n"
            "  cmake -B build && cmake --build build\n",
            file=sys.stderr,
        )
        sys.exit(1)

    id_bytes  = obfuscate(client_id)

    header = f"""\
// AUTO-GENERATED — do not edit.
// Produced by scripts/gen_credentials.py at build time.
// Credentials are XOR-obfuscated with key 0x{XOR_KEY:02X}.
// To decode:  for each byte b in the array, the real char is b ^ 0x{XOR_KEY:02X}.
#pragma once
#include <string>

// XOR key used during obfuscation
static constexpr unsigned char CRED_XOR_KEY = 0x{XOR_KEY:02X};

static const unsigned char SPOTIFY_CLIENT_ID_OBF[] = {{ {fmt_bytes(id_bytes)} }};

// Deobfuscates an obfuscated credential array into a std::string at runtime.
inline std::string DeobfuscateCredential(const unsigned char *data, size_t len) {{
    std::string result(len, '\\0');
    for (size_t i = 0; i < len; ++i)
        result[i] = static_cast<char>(data[i] ^ CRED_XOR_KEY);
    return result;
}}

inline std::string GetSpotifyClientId() {{
    return DeobfuscateCredential(SPOTIFY_CLIENT_ID_OBF, sizeof(SPOTIFY_CLIENT_ID_OBF));
}}

"""

    os.makedirs(output_dir, exist_ok=True)
    out_path = os.path.join(output_dir, "credentials.h")
    with open(out_path, "w") as f:
        f.write(header)

    print(f"credentials.h written to {out_path}")


if __name__ == "__main__":
    main()
