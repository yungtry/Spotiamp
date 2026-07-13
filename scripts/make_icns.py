#!/usr/bin/env python3
import os
import shutil
import struct
import subprocess
import sys
import tempfile


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def extract_largest_png_from_ico(ico_path, png_path):
    with open(ico_path, "rb") as f:
        data = f.read()

    if len(data) < 6:
        raise RuntimeError("ICO file is too small")

    reserved, icon_type, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or icon_type != 1 or count < 1:
        raise RuntimeError("Not a valid ICO file")

    best = None
    for index in range(count):
        offset = 6 + index * 16
        if offset + 16 > len(data):
            break

        width, height, _colors, _reserved, _planes, bit_count, size, image_offset = struct.unpack_from(
            "<BBBBHHII", data, offset
        )
        width = 256 if width == 0 else width
        height = 256 if height == 0 else height
        blob = data[image_offset : image_offset + size]
        if not blob.startswith(PNG_SIGNATURE):
            continue

        score = (width * height, bit_count, size)
        if best is None or score > best[0]:
            best = (score, blob)

    if best is None:
        raise RuntimeError("ICO does not contain a PNG-compressed icon")

    with open(png_path, "wb") as f:
        f.write(best[1])


def run(command):
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def write_icns(png_entries, icns_path):
    chunks = []
    for icon_type, png_path in png_entries:
        with open(png_path, "rb") as f:
            payload = f.read()
        chunks.append(icon_type.encode("ascii") + struct.pack(">I", len(payload) + 8) + payload)

    total_size = 8 + sum(len(chunk) for chunk in chunks)
    with open(icns_path, "wb") as f:
        f.write(b"icns")
        f.write(struct.pack(">I", total_size))
        for chunk in chunks:
            f.write(chunk)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_icns.py input.ico output.icns")

    ico_path = os.path.abspath(sys.argv[1])
    icns_path = os.path.abspath(sys.argv[2])

    if shutil.which("sips") is None:
        raise SystemExit("sips is required to generate a macOS .icns file")

    os.makedirs(os.path.dirname(icns_path), exist_ok=True)

    with tempfile.TemporaryDirectory() as temp_dir:
        source_png = os.path.join(temp_dir, "source.png")
        extract_largest_png_from_ico(ico_path, source_png)

        sizes = [
            (16, "icp4"),
            (32, "icp5"),
            (64, "icp6"),
            (128, "ic07"),
            (256, "ic08"),
            (512, "ic09"),
            (1024, "ic10"),
        ]

        png_entries = []
        for size, icon_type in sizes:
            png_path = os.path.join(temp_dir, f"{size}.png")
            run(
                [
                    "sips",
                    "-s",
                    "format",
                    "png",
                    "-z",
                    str(size),
                    str(size),
                    source_png,
                    "--out",
                    png_path,
                ]
            )
            png_entries.append((icon_type, png_path))

        write_icns(png_entries, icns_path)


if __name__ == "__main__":
    main()
