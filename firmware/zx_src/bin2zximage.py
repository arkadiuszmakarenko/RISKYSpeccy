#!/usr/bin/env python3
"""
bin2zximage.py - Convert a Z80 binary to firmware/User/zx_image.c

Usage:
    python3 bin2zximage.py <input.bin> <output/zx_image.c>

The output file defines:
    const uint8_t  g_zx_image[16384];
    const uint32_t g_zx_image_size;

Binary is zero-padded to exactly 16384 bytes.
"""

import sys
import os

ROM_SIZE = 16384
BYTES_PER_LINE = 16

HEADER = """\
/* AUTO-GENERATED — do not edit by hand.
 * Rebuilt by: make -C firmware/zx_src
 */
#include "zx_image.h"

const uint8_t g_zx_image[16384] = {
"""

FOOTER = """\
};

const uint32_t g_zx_image_size = 16384u;
"""


def convert(bin_path, out_path):
    with open(bin_path, "rb") as f:
        data = bytearray(f.read())

    if len(data) > ROM_SIZE:
        print(f"Warning: binary is {len(data)} bytes, truncating to {ROM_SIZE}",
              file=sys.stderr)
        data = data[:ROM_SIZE]
    elif len(data) < ROM_SIZE:
        data.extend(bytes(ROM_SIZE - len(data)))

    with open(out_path, "w") as f:
        f.write(HEADER)
        for offset in range(0, ROM_SIZE, BYTES_PER_LINE):
            chunk = data[offset:offset + BYTES_PER_LINE]
            hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
            f.write(f"    /* 0x{offset:04X} */ {hex_vals},\n")
        f.write(FOOTER)

    print(f"Written {out_path} ({len(data)} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {os.path.basename(sys.argv[0])} <input.bin> <output_zx_image.c>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
