#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys


DEFAULT_DIRS = ("bios", "cores", "fonts", "font", "roms")


def parse_int(value):
    return int(str(value), 0)


def is_md_rom(dest, rel_path):
    return dest == "roms" and len(rel_path.parts) > 1 and rel_path.parts[0].lower() == "md" and rel_path.name != ".DS_Store"


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        dst.unlink()
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def copy_byteswapped_16(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    with src.open("rb") as in_file, dst.open("wb") as out_file:
        pending = b""
        while True:
            chunk = in_file.read(1024 * 1024)
            if not chunk:
                break

            if pending:
                chunk = pending + chunk
                pending = b""

            if len(chunk) & 1:
                pending = chunk[-1:]
                chunk = chunk[:-1]

            data = bytearray(chunk)
            data[0::2], data[1::2] = data[1::2], data[0::2]
            out_file.write(data)

        if pending:
            out_file.write(pending)
    shutil.copystat(src, dst)


def stage_input_dirs(collect_dirs, stage_dir):
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True)

    staged_dirs = []
    byteswapped_count = 0
    for src, dest in collect_dirs:
        staged_root = stage_dir / dest
        staged_root.mkdir(parents=True, exist_ok=True)
        staged_dirs.append((staged_root, dest))

        for path in src.rglob("*"):
            rel_path = path.relative_to(src)
            staged_path = staged_root / rel_path
            if path.is_dir():
                staged_path.mkdir(parents=True, exist_ok=True)
            elif path.is_file():
                if is_md_rom(dest, rel_path):
                    copy_byteswapped_16(path, staged_path)
                    byteswapped_count += 1
                else:
                    link_or_copy(path, staged_path)

    return staged_dirs, byteswapped_count


def main():
    parser = argparse.ArgumentParser(description="Generate a FrogFS image from sd_content.")
    parser.add_argument("--sd-content", default="sd_content", help="Source sd_content directory")
    parser.add_argument("--output", default="build/frogfs.bin", help="Output FrogFS image")
    parser.add_argument("--build-dir", default="build/frogfs", help="FrogFS build/cache directory")
    parser.add_argument(
        "--include",
        action="append",
        default=[],
        help="Top-level sd_content directory to include. May be repeated. Defaults to bios, cores, fonts/font and roms when present.",
    )
    parser.add_argument("--roms-dir", default="roms", help="Project root ROM directory to merge into /roms when present")
    parser.add_argument("--reserve-size", type=parse_int, default=0, help="Optional maximum reserved size in bytes")
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parents[1]
    sd_content = (repo / args.sd_content).resolve()
    output = (repo / args.output).resolve()
    build_dir = (repo / args.build_dir).resolve()
    mkfrogfs = repo / "Core/Src/porting/lib/frogfs/tools/mkfrogfs.py"

    if not mkfrogfs.exists():
        print(f"FrogFS tool not found: {mkfrogfs}", file=sys.stderr)
        return 1

    if not sd_content.is_dir():
        print(f"sd_content directory not found: {sd_content}", file=sys.stderr)
        return 1

    requested_dirs = tuple(args.include) if args.include else DEFAULT_DIRS
    collect_dirs = []

    project_roms = (repo / args.roms_dir).resolve()
    if project_roms.is_dir():
        collect_dirs.append((project_roms, "roms"))

    for dirname in requested_dirs:
        src = sd_content / dirname
        if src.is_dir():
            collect_dirs.append((src, dirname))

    if not collect_dirs:
        print(f"No FrogFS input directories found in {sd_content}", file=sys.stderr)
        return 1

    build_dir.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)

    staged_dirs, byteswapped_count = stage_input_dirs(collect_dirs, build_dir / "input")

    config_path = build_dir / "frogfs.yaml"
    with config_path.open("w", encoding="utf-8") as config:
        config.write("collect:\n")
        for src, dest in staged_dirs:
            source = str(src).replace(os.sep, "/") + "/"
            config.write(f"  - {json.dumps(source)}: {json.dumps('/' + dest)}\n")
        config.write("\nfilter:\n")
        config.write("  '*/.DS_Store':\n")
        config.write("    - discard\n")
        config.write("  '.DS_Store':\n")
        config.write("    - discard\n")

    cmd = [
        sys.executable,
        str(mkfrogfs),
        "-C",
        str(repo),
        str(config_path),
        str(build_dir),
        str(output),
    ]
    subprocess.check_call(cmd)

    size = output.stat().st_size
    if args.reserve_size and size > args.reserve_size:
        print(
            f"FrogFS image is {size} bytes, larger than reserved size {args.reserve_size} bytes",
            file=sys.stderr,
        )
        return 1

    dirs = ", ".join("/" + dest for _, dest in collect_dirs)
    print(f"Generated {output} ({size} bytes) from {dirs}")
    if byteswapped_count:
        print(f"Byteswapped {byteswapped_count} file(s) under /roms/md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
