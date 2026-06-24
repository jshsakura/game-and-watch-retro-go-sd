# Cover-art decode repro / regression

Links the **real** device cover decoders (`music_cover.c`, `tjpgd.c`,
`progjpeg.c`, `music_lupng.c`, miniz) on the host and renders a 4-quadrant
reference cover (red/green/blue/yellow) in every format, then reads the colors
back from the framebuffer. Catches channel-order / R<->B swap regressions.

## Run
```sh
python3 host/cover_repro/gen_images.py
gcc -O2 -std=gnu11 -w -Ihost -ICore/Inc/porting/music \
    -Iretro-go-stm32/components/lupng \
    host/cover_repro/driver.c Core/Src/porting/music/music_cover.c \
    Core/Src/porting/music/music_lupng.c Core/Src/porting/music/progjpeg.c \
    Core/Src/porting/music/tjpgd.c retro-go-stm32/components/lupng/miniz.c \
    -o host/cover_repro/driver
for n in jpgbase jpgprog pngrgb pngrgba pngpal; do
  host/cover_repro/driver host/cover_repro/imgs/$n.mp3 host/cover_repro/out/$n.bin
done
python3 host/cover_repro/check.py host/cover_repro/out/*.bin
```

## Bug this caught
`tjpgd.c` `jd_mcu_output()` emitted the RGB888 intermediate as **B,G,R** while
the RGB565 packer puts the first byte in the high/red bits -> baseline & all
TJpgDec-decoded JPEG covers rendered as **BGR (red<->blue swapped)**. PNG covers
(lupng, true RGB) were unaffected, so only JPEG art looked wrong. Fixed by
emitting R,G,B (the stock ChaN ordering).
