# Third-party icon attribution

Some launcher system icons are rendered from the [RomM](https://github.com/rommapp/romm)
project's `systematic` icon set, via `tools/gen_color_icons.py`, from the copies kept in
the companion project `game-and-what` under
`frontend/public/system-icons/`.

Those icons are licensed **CC BY 4.0**
(<https://creativecommons.org/licenses/by/4.0/>). Attribution is required, and this file
is it.

| Firmware symbol | Source file | Upstream |
|---|---|---|
| `cicon_cps1` (`RG_LOGO_PAD_CPS1`) | `cps1.svg` | RomM `systematic/cps1.svg`, CC BY 4.0 |
| `cicon_32x` (`RG_LOGO_PAD_32X`) | `32x.svg` | RomM icon set |

Other icons in `Core/Src/retro-go/rg_logos.c` are either original to this project or
predate the RomM pipeline; `tools/gen_color_icons.py`'s `MAP` is the authoritative list of
which symbols are generated from that source directory.

If you add an icon from the RomM set, add a row here in the same commit. Shipping the
artwork without the attribution is a licence violation, not a formatting oversight.
