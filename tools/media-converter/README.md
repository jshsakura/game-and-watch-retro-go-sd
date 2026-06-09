# G&W Media Converter

A single-page web tool that converts arbitrary media into files the on-device
**Media** browser can open. Drag files in, download the converted versions,
copy them to the SD card's `/media` folder.

| Input | Output | What it does |
|-------|--------|--------------|
| Images (jpg/png/webp/heic/…) | `.jpg` (or `.png`) | Shrinks to fit 320×240, keeping aspect (no upscaling). JPG is preferred — it's hardware-decoded on the device. |
| Text (`.txt`) | UTF-8 `.txt` | Auto-detects the encoding (handles **EUC-KR/CP949** Korean) and re-saves as UTF-8, which the viewer needs. |
| Video (mp4/mkv/mov/…) | `.avi` | Transcodes to **MJPEG + PCM** via ffmpeg.wasm, using the exact recipe the device expects (320×240, 30 fps, mono 24 kHz). |

This removes the device-side constraints (image size, text encoding, video
codec): the firmware only ever sees correctly-sized, correctly-encoded files.

## Run it

It's a static page, but the **video** converter pulls `ffmpeg.wasm` from a CDN,
so it needs to be served over http(s) (not opened as a `file://` URL). Any of:

```bash
# from this folder
python3 -m http.server 8000
# then open http://localhost:8000
```

or host the folder on GitHub Pages / Netlify / Vercel / Cloudflare Pages.

Notes:
- Uses the **single-threaded** ffmpeg core, so it works without
  cross-origin-isolation (COOP/COEP) headers.
- Image and text conversion run fully offline; only video needs the network
  (first run downloads ~30 MB of ffmpeg.wasm, then it's cached).
- Everything runs in your browser — no files are uploaded anywhere.

## Equivalent ffmpeg CLI (if you prefer the command line)

```bash
ffmpeg -i input.mp4 -c:v mjpeg -q:v 5 \
  -vf scale=w=320:h=240:force_original_aspect_ratio=decrease:force_divisible_by=16,format=yuv420p,scale=src_range=1:dst_range=1 \
  -r 30 -c:a pcm_s16le -ac 1 -ar 24000 output.avi
```
