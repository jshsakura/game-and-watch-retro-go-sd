from __future__ import annotations

import os
import argparse
from io import BytesIO
from pathlib import Path
from typing import Optional

from PIL import Image

# --- PICO-8 cart label (.p8.png): 128x128 region at (16,24) in 160x205 PNG ---

PICO8_PALETTE = [
    (0, 0, 0),
    (29, 43, 83),
    (126, 37, 83),
    (0, 135, 81),
    (171, 82, 54),
    (95, 87, 79),
    (194, 195, 199),
    (255, 241, 232),
    (255, 0, 77),
    (255, 163, 0),
    (255, 236, 39),
    (0, 228, 54),
    (41, 173, 255),
    (131, 118, 156),
    (255, 119, 168),
    (255, 204, 170),
]

PICO8_PALETTE_EXT = [
    (41, 24, 20),
    (17, 29, 53),
    (66, 33, 54),
    (18, 83, 89),
    (116, 47, 41),
    (73, 51, 59),
    (162, 136, 121),
    (243, 239, 125),
    (190, 18, 80),
    (255, 108, 36),
    (168, 231, 46),
    (0, 181, 67),
    (6, 90, 181),
    (117, 70, 101),
    (255, 110, 89),
    (255, 157, 129),
]

PICO8_FULL_PALETTE = PICO8_PALETTE + PICO8_PALETTE_EXT


def _is_pico8_png(path: Path) -> bool:
    return path.name.lower().endswith(".p8.png")


def _pico8_closest_palette_index(r, g, b):
    best_idx = 0
    best_dist = 10**12
    for i, (pr, pg, pb) in enumerate(PICO8_FULL_PALETTE):
        dr, dg, db = pr - r, pg - g, pb - b
        dist = dr * dr + dg * dg + db * db
        if dist < best_dist:
            best_dist = dist
            best_idx = i
    return best_idx


def extract_pico8_label_from_cart_png(srcfile) -> Optional[Image.Image]:
    """128x128 label from embedded cart PNG, snapped to PICO-8 palette."""
    try:
        src = Image.open(srcfile).convert("RGB")
    except OSError as e:
        print(f"Error: cannot open PICO-8 PNG {srcfile}: {e}")
        return None

    w, h = src.size
    lx, ly = 16, 24
    side = 128
    if w < lx + side or h < ly + side:
        print(
            f"Error: PICO-8 PNG too small ({w}x{h}), need ≥{lx + side}x{ly + side}: {srcfile}"
        )
        return None

    out = Image.new("RGB", (side, side))
    op = out.load()
    sp = src.load()
    for y in range(side):
        for x in range(side):
            r, g, b = sp[lx + x, ly + y]
            idx = _pico8_closest_palette_index(r, g, b)
            op[x, y] = PICO8_FULL_PALETTE[idx]
    return out


def compare_versions(version1, version2):
    """
    Compare two versions represented as strings.
    Returns:
        -1 if version1 < version2
         0 if version1 == version2
         1 if version1 > version2
    """
    v1_parts = [int(part) for part in version1.split('.')]
    v2_parts = [int(part) for part in version2.split('.')]
    
    for v1, v2 in zip(v1_parts, v2_parts):
        if v1 < v2:
            return -1
        elif v1 > v2:
            return 1
    
    if len(v1_parts) < len(v2_parts):
        return -1
    elif len(v1_parts) > len(v2_parts):
        return 1
    else:
        return 0

def calculate_new_size(img, target_width=None, target_height=None):
    """
    Calculate the new size maintaining the aspect ratio.
    Ensures the entire image fits within the specified dimensions.
    """
    MAX_WIDTH, MAX_HEIGHT = 186, 100
    original_width, original_height = img.size
    
    # Set target dimensions with respect to the max constraints
    if target_width is None:
        target_width = MAX_WIDTH
    if target_height is None:
        target_height = MAX_HEIGHT
    
    target_width = min(target_width, MAX_WIDTH)
    target_height = min(target_height, MAX_HEIGHT)
    
    scale_w = target_width / original_width
    scale_h = target_height / original_height
    scale = min(scale_w, scale_h)  # Use the smallest scale to fit entirely
    
    new_width = int(original_width * scale)
    new_height = int(original_height * scale)
    
    return new_width, new_height

def _lanczos_resample():
    if compare_versions(Image.__version__, "7.0") >= 0:
        return Image.Resampling.LANCZOS
    return Image.ANTIALIAS


def _save_jpeg_rgb(
    img: Image.Image,
    output_file,
    jpg_quality: int,
    max_jpeg_bytes: Optional[int],
):
    """Write RGB image as JPEG; optionally lower quality until file size ≤ max_jpeg_bytes."""
    if max_jpeg_bytes is None:
        img.save(output_file, format="JPEG", optimize=True, quality=jpg_quality)
        return

    q = max(1, min(100, jpg_quality))
    min_q = 25
    step = 6
    best: Optional[bytes] = None
    while q >= min_q:
        buf = BytesIO()
        img.save(buf, format="JPEG", optimize=True, quality=q)
        data = buf.getvalue()
        if best is None or len(data) < len(best):
            best = data
        if len(data) <= max_jpeg_bytes:
            Path(output_file).write_bytes(data)
            return
        q -= step

    assert best is not None
    Path(output_file).write_bytes(best)
    if len(best) > max_jpeg_bytes:
        print(
            f"Warning: {output_file} is {len(best)} bytes "
            f"(target <={max_jpeg_bytes}); quality lowered to minimum ({min_q})"
        )


def write_thumbnail(
    srcfile,
    output_file,
    target_width,
    target_height,
    jpg_quality,
    *,
    max_jpeg_bytes: Optional[int] = None,
    from_image: Optional[Image.Image] = None,
):
    """
    Create a thumbnail and save as JPEG (aspect ratio preserved).
    If from_image is set, pixels come from it; otherwise srcfile is opened.
    """
    resample = _lanczos_resample()
    if from_image is not None:
        img = from_image
    else:
        img = Image.open(srcfile).convert(mode="RGB")
    new_size = calculate_new_size(img, target_width, target_height)
    img = img.resize(new_size, resample)
    _save_jpeg_rgb(img, output_file, jpg_quality, max_jpeg_bytes)

def process_single_image(
    image_file,
    output_file,
    width=None,
    height=None,
    jpg_quality=85,
    *,
    pico8_jpeg_max_bytes: Optional[int] = 10240,
):
    """
    Process a single image file and create a thumbnail.
    For *.p8.png, extracts the 128x128 cart label (PICO-8 palette) before scaling.
    pico8_jpeg_max_bytes: max JPEG size for those files (lower quality if needed);
        None or <=0 disables the cap.
    """
    img_path = Path(image_file)
    if not img_path.exists():
        print(f"Error: Image file {image_file} does not exist.")
        return False

    if img_path.suffix.lower() not in [".png", ".jpg", ".jpeg", ".bmp"]:
        print(
            "Error: Unsupported image format. Supported formats: .png, .jpg, .jpeg, .bmp"
        )
        return False

    output_path = Path(output_file)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if output_path.exists():
        print(f"Thumbnail already exists at {output_path}, skipping...")
        return True

    if pico8_jpeg_max_bytes is None or pico8_jpeg_max_bytes <= 0:
        cap = None
    else:
        cap = pico8_jpeg_max_bytes

    if _is_pico8_png(img_path):
        label = extract_pico8_label_from_cart_png(img_path)
        if label is None:
            return False
        print(f"Creating PICO-8 label thumbnail for {img_path} → {output_path}...")
        write_thumbnail(
            img_path,
            output_path,
            width,
            height,
            jpg_quality,
            max_jpeg_bytes=cap,
            from_image=label,
        )
    else:
        print(f"Creating thumbnail for {img_path} and saving to {output_path}...")
        write_thumbnail(img_path, output_path, width, height, jpg_quality)

    return True


def process_images_in_roms(
    roms_directory,
    covers_directory,
    width=None,
    height=None,
    jpg_quality=85,
    *,
    pico8_jpeg_max_bytes: Optional[int] = 10240,
):
    """
    Process all images in subdirectories of the roms directory and create thumbnails
    in the corresponding subdirectories under covers_directory, only if they don't exist.
    """
    if pico8_jpeg_max_bytes is None or pico8_jpeg_max_bytes <= 0:
        cap = None
    else:
        cap = pico8_jpeg_max_bytes

    for subdir, _, files in os.walk(roms_directory):
        for file in files:
            img_path = Path(subdir) / file
            if img_path.suffix.lower() in [".png", ".jpg", ".jpeg", ".bmp"]:
                relative_path = img_path.relative_to(roms_directory)
                output_subdir = Path(covers_directory) / relative_path.parent
                output_subdir.mkdir(parents=True, exist_ok=True)
                output_file = output_subdir / (img_path.stem + ".img")

                if output_file.exists():
                    print(f"Thumbnail already exists for {img_path}, skipping...")
                    continue

                if _is_pico8_png(img_path):
                    label = extract_pico8_label_from_cart_png(img_path)
                    if label is None:
                        continue
                    print(
                        f"Creating PICO-8 label thumbnail for {img_path} → {output_file}..."
                    )
                    write_thumbnail(
                        img_path,
                        output_file,
                        width,
                        height,
                        jpg_quality,
                        max_jpeg_bytes=cap,
                        from_image=label,
                    )
                else:
                    print(
                        f"Creating thumbnail for {img_path} and saving to {output_file}..."
                    )
                    write_thumbnail(
                        img_path, output_file, width, height, jpg_quality
                    )

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate thumbnails for images.")
    parser.add_argument("--src", type=str, default="roms", help="Path to the source directory")
    parser.add_argument("--dst", type=str, default="covers", help="Path to the dest covers directory")
    parser.add_argument("--width", type=int, default=128, help="Thumbnail width (set to None to only use height-based scaling)")
    parser.add_argument("--height", type=int, default=None, help="Thumbnail height (set to None to only use width-based scaling)")
    parser.add_argument("--jpg_quality", type=int, default=85, help="JPEG quality (0-100)")
    parser.add_argument("--image", type=str, help="Path to a single image file to process")
    parser.add_argument(
        "--pico8_jpeg_max_bytes",
        type=int,
        default=10240,
        help="For *.p8.png only: max output JPEG size in bytes (default 10240). "
        "If exceeded, quality is reduced. Use 0 for no limit.",
    )
    
    args = parser.parse_args()
    
    pico8_cap = None if args.pico8_jpeg_max_bytes == 0 else args.pico8_jpeg_max_bytes
    
    # If --image is specified, process a single image
    if args.image:
        if not args.output:
            # If no output specified, create output filename based on input
            input_path = Path(args.image)
            args.output = input_path.parent / (input_path.stem + ".img")
        
        success = process_single_image(
            args.image,
            args.output,
            args.width,
            args.height,
            args.jpg_quality,
            pico8_jpeg_max_bytes=pico8_cap,
        )
        if success:
            print(f"Successfully created thumbnail: {args.output}")
        else:
            print("Failed to create thumbnail.")
    else:
        # Process all images in the source directory
        process_images_in_roms(
            args.src,
            args.dst,
            args.width,
            args.height,
            args.jpg_quality,
            pico8_jpeg_max_bytes=pico8_cap,
        )
