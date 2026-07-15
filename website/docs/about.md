---
id: about
title: About & credits
---

# About & credits

## How it's built (the honest part)

I don't come from an embedded background and I don't debug with JTAG. I'm a web developer, and
almost all of the low-level work here is done with Claude (AI-assisted). That's also why none of
this is pushed upstream directly: it isn't clean enough for that, and I can't personally vouch for
every line — so it stays here, in my own lab, shared only so people can see this approach is even
possible.

What keeps it honest is two things: **host harnesses** under `linux/<sys>/` that link the *same
core source* the firmware ships and reproduce bugs deterministically on a PC, and **SD-card logs**
read back from the real hardware. Everything was tested by one person on one device, so it will not
be perfect. Findings are kept fact-based and tagged by how they were verified.

## 🙏 A note to upstream and the community

This is an **experimental fork, nothing more.** If any idea here turns out to be useful, **please
take it upstream** — open a PR or a feature request on
[sylverb's repository](https://github.com/sylverb/game-and-watch-retro-go-sd) and let the stable
project decide what fits. I'd be genuinely happy if any part of this helps you or the Game & Watch
community. Everything here is offered in that spirit, with respect and gratitude to sylverb and the
retro-go contributors — without their work, none of this would exist.

## Companion: game-and-what {#companion-game-and-what}

The web tool **[jshsakura/game-and-what](https://github.com/jshsakura/game-and-what)** — a sibling
project by the same author, kept in the parent folder (`../game-and-what`) next to the firmware repo
— prepares the assets this firmware uses:

- renders the per-system launcher **icons**,
- encodes any image or video into a **clock-ready 320×240 GIF** (`encode_to_clock_gif`),
- generates the **GBA idle-loop / cart tables** the emulator ships with.

It's a separate web app, **configured through its own env variables**, and the asset-generation
scripts in the firmware repo read from it (defaulting to the `../game-and-what` sibling folder). It
exists to feed this fork; you don't need it to *use* the firmware, only to make new assets.

## License

This project uses Fusion Pixel Font (SIL Open Font License 1.1).

This project is licensed under the GPLv2. Some components are available under the MIT license.
Respective copyrights apply to each component.
