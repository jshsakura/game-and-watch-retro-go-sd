# Third-party components (Media / Music app)

All vendored libraries are permissive-licensed (compatible with the project) and
keep their original copyright/license headers in the source files.

| Component | Files | License | Notes |
|-----------|-------|---------|-------|
| **minimp3** (lieff) | `minimp3.h`, `media_minimp3.c` | CC0 / public domain | MP3 decoder. Unmodified header. |
| **TJpgDec R0.03** (ChaN) | `tjpgd.c`, `tjpgd.h`, `tjpgdcnf.h` | ChaN free-software license ("no restriction on use … under your responsibility") | Scaled JPEG decoder for cover art. Config set to RGB565 + scaling; LVGL include wrappers stripped. |
| **lupng** (Jan Solanti) | `media_lupng.c` | MIT | PNG decoder. Vendored copy of the submodule's `lupng.c`; patched to use miniz's names and to fix a missing `_error` label. MIT header retained. |
| **miniz** (Rich Geldreich / RAD / Valve) | `retro-go-stm32/components/lupng/miniz.c` | MIT | DEFLATE for lupng. From the existing submodule. |

None of these are copyleft (GPL/LGPL), so they impose no additional obligations
beyond retaining their notices, which is done in-file.
