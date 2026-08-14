#!/usr/bin/env python3

# This script allows to convert raw 320x240 framebuffer dumps into png files
# It can be used to convert savestates .raw screenshot into png.
import sys
from PIL import Image
import numpy as np
import os

def convert_rgb565_to_png(input_file, width=320, height=240):
    output_file = os.path.splitext(input_file)[0] + ".png"
    
    with open(input_file, "rb") as f:
        raw_data = f.read()

    data = np.frombuffer(raw_data, dtype=np.uint16)

    red = ((data >> 11) & 0x1F) * 255 // 31
    green = ((data >> 5) & 0x3F) * 255 // 63
    blue = (data & 0x1F) * 255 // 31

    rgb_array = np.stack((red, green, blue), axis=-1).reshape((height, width, 3))

    image = Image.fromarray(rgb_array.astype('uint8'), 'RGB')
    image.save(output_file)
    print(f"Image saved as {output_file}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python binary_rgb565_to_png.py <binary file name> [width] [height]")
        sys.exit(1)

    # Dimensions default to the panel's 320x240, which is every savestate
    # screenshot. screenshot.sh passes them explicitly because it reads the
    # real geometry out of LTDC and must not silently reshape a frame that
    # is not that size.
    input_file = sys.argv[1]
    width = int(sys.argv[2]) if len(sys.argv) > 2 else 320
    height = int(sys.argv[3]) if len(sys.argv) > 3 else 240
    convert_rgb565_to_png(input_file, width, height)
