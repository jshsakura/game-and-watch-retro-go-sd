#!/usr/bin/env python3
"""Prepare a Super Metroid ROM for the Game & Watch port.

Zelda 3 and Super Mario World are shipped as an assets file built from the ROM.
Super Metroid is not: the port reads the original ROM at runtime, so what the SD
card needs is the ROM itself, at /roms/homebrew/sm.smc, headerless.

That "headerless" is the whole reason this script exists. Half the dumps in
circulation carry a 512-byte copier header, and the port indexes the image as a
flat LoROM (bank N at file offset N * 0x8000). Feed it a headered file and every
read is 512 bytes off: the language probe reads garbage and the game dies with no
message that means anything. Nothing in the firmware checks for it -- so check
here, where we can still say why.

Usage:
    python3 tools/sm_prepare_rom.py <rom> [-o sm.smc]

Then copy sm.smc into /roms/homebrew/ on the SD card.
"""

import argparse
import hashlib
import sys
from pathlib import Path

# snesrev/sm's reference ROM (its README pins this hash).
STOCK_SHA1 = "da957f0d63d14cb441d215462904c4fa8519c613"

ROM_SIZE = 3 * 1024 * 1024  # 3 MiB, headerless
COPIER_HEADER_SIZE = 512

# The port reads the second language's text out of the ROM, so a fan patch (e.g.
# the Korean one) is a legitimate input with a hash we cannot know in advance.
# Size and shape are what we can actually verify.


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def strip_copier_header(data: bytes) -> tuple[bytes, bool]:
    """Return (payload, had_header). A copier header is 512 bytes of prefix."""
    if len(data) == ROM_SIZE + COPIER_HEADER_SIZE:
        return data[COPIER_HEADER_SIZE:], True
    return data, False


def describe_rom(payload: bytes) -> str:
    sha1 = hashlib.sha1(payload).hexdigest()
    if sha1 == STOCK_SHA1:
        return "stock ROM -- the port's second language will be Japanese"
    return (
        f"sha1 {sha1}\n"
        "         not the stock ROM. If this is a fan patch (e.g. the Korean\n"
        "         translation), that is expected and supported: the port shows the\n"
        "         patched text as its second language. If you did not patch\n"
        "         anything, you have the wrong dump."
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rom", type=Path, help="Super Metroid ROM (.smc/.sfc), headered or not")
    parser.add_argument("-o", "--output", type=Path, default=Path("sm.smc"),
                        help="output path (default: sm.smc)")
    args = parser.parse_args()

    if not args.rom.is_file():
        fail(f"no such file: {args.rom}")

    data = args.rom.read_bytes()
    payload, had_header = strip_copier_header(data)

    if len(payload) != ROM_SIZE:
        fail(
            f"{args.rom} is {len(data):,} bytes; a Super Metroid ROM is "
            f"{ROM_SIZE:,} (or {ROM_SIZE + COPIER_HEADER_SIZE:,} with a copier header). "
            "This is not the right file."
        )

    if had_header:
        print(f"  input:  {args.rom} (512-byte copier header found -- stripping it)")
    else:
        print(f"  input:  {args.rom} (no copier header)")
    print(f"  rom:    {describe_rom(payload)}")

    args.output.write_bytes(payload)
    print(f"  wrote:  {args.output} ({len(payload):,} bytes)")
    print()
    print(f"Copy {args.output} to /roms/homebrew/ on the SD card.")


if __name__ == "__main__":
    main()
