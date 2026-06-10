#!/usr/bin/env python3
"""
Post-process frogfs.bin: locate uncompressed cores/pico8.ro payload and relocate
sentinel words for the real XiP address (__EXTFLASH_START__ + data_offs).

Layout matches Core/Src/porting/lib/frogfs/src/frogfs_format.h and frogfs.c (djb2 hash).
"""
from __future__ import annotations

import pathlib
import struct
import sys
import zlib

from pico8_ro_build_patch import patch_pico8_ro_bytes

FROGFS_MAGIC = 0x474F5246


def djb2_hash_c(path: str) -> int:
    h = 5381
    for c in path.encode("utf-8"):
        h = (((h << 5) + h) ^ c) & 0xFFFFFFFF
    return h


def _is_dir(child_count: int) -> bool:
    return child_count < 0xFF00


def _is_file_uncompressed(child_count: int) -> bool:
    return child_count == 0xFF00


def _is_compressed(child_count: int) -> bool:
    return child_count > 0xFF00


def _name_bytes(img: bytes, entry_off: int) -> bytes:
    _parent, cc, seg_sz, _opts = struct.unpack_from("<IHBB", img, entry_off)
    if _is_dir(cc):
        name_start = entry_off + 8 + cc * 4
    elif _is_file_uncompressed(cc):
        name_start = entry_off + 16
    else:
        name_start = entry_off + 20
    return img[name_start : name_start + seg_sz]


def frogfs_get_path(img: bytes, entry_off: int) -> str:
    segs: list[str] = []
    cur = entry_off
    while True:
        parent_off, cc, seg_sz, _opts = struct.unpack_from("<IHBB", img, cur)
        segs.append(_name_bytes(img, cur).decode("utf-8"))
        if parent_off == 0:
            break
        cur = parent_off
    return "/".join(s for s in reversed(segs) if s)


def find_uncompressed_file_payload(img: bytes, want_path: str) -> tuple[int, int] | None:
    want_path = want_path.lstrip("/")
    if len(img) < 12:
        return None
    magic, _ma, _mi, num_ent, _bin_sz = struct.unpack_from("<IBBHI", img, 0)
    if magic != FROGFS_MAGIC:
        return None

    h_seek = djb2_hash_c(want_path)
    hash_base = 12
    pairs: list[tuple[int, int]] = []
    for i in range(num_ent):
        hv, offs = struct.unpack_from("<II", img, hash_base + i * 8)
        pairs.append((hv, offs))

    lo, hi = 0, num_ent - 1
    found_mid = -1
    while lo <= hi:
        mid = (lo + hi) // 2
        if pairs[mid][0] < h_seek:
            lo = mid + 1
        elif pairs[mid][0] > h_seek:
            hi = mid - 1
        else:
            found_mid = mid
            break
    if found_mid < 0:
        return None

    mid = found_mid
    while mid > 0 and pairs[mid - 1][0] == h_seek:
        mid -= 1

    while mid < num_ent and pairs[mid][0] == h_seek:
        entry_off = pairs[mid][1]
        _parent, cc, _seg_sz, _opts = struct.unpack_from("<IHBB", img, entry_off)
        if _is_compressed(cc):
            print(
                f"frogfs_pico8_ro: '{want_path}' is compressed; need uncompressed for XIP",
                file=sys.stderr,
            )
            return None
        if _is_file_uncompressed(cc):
            path = frogfs_get_path(img, entry_off)
            if path == want_path:
                data_offs, data_sz = struct.unpack_from("<II", img, entry_off + 8)
                if data_offs + data_sz > len(img):
                    return None
                return data_offs, data_sz
        mid += 1
    return None


def patch_frogfs_pico8_ro_inplace(
    frogfs_bin_path: str | pathlib.Path,
    *,
    logical_path: str = "cores/pico8.ro",
    extflash_base: int = 0x90000000,
    extflash_offset: int = 0,
) -> bool:
    logical_path = logical_path.lstrip("/")
    path = pathlib.Path(frogfs_bin_path)
    img = bytearray(path.read_bytes())
    found = find_uncompressed_file_payload(bytes(img), logical_path)
    if not found:
        print(f"frogfs_pico8_ro: '{logical_path}' not found in image", file=sys.stderr)
        return False
    data_offs, data_sz = found
    target_addr = extflash_base + extflash_offset + data_offs
    payload = bytes(img[data_offs : data_offs + data_sz])
    blob, n = patch_pico8_ro_bytes(payload, target_addr)
    if n == 0:
        print(
            "frogfs_pico8_ro: warning: 0 sentinel refs patched "
            "(unexpected for upstream pico8.ro)",
            file=sys.stderr,
        )
    else:
        print(f"frogfs_pico8_ro: patched {n} refs for XiP 0x{target_addr:X} ({logical_path})")
    img[data_offs : data_offs + data_sz] = blob
    if len(img) < 4:
        return False
    crc = zlib.crc32(bytes(img[:-4])) & 0xFFFFFFFF
    struct.pack_into("<I", img, len(img) - 4, crc)
    path.write_bytes(bytes(img))
    return True
