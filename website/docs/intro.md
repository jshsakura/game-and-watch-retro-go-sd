---
id: intro
title: Overview & install
sidebar_position: 1
slug: /intro
---

# 🧪 Experimental Lab Fork

A **personal experimental lab** built on top of
[sylverb/game-and-watch-retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd),
the excellent SD-card fork of retro-go for the Nintendo® Game & Watch™.

It's rough around the edges and a bit of a mess — this is a personal lab, not a "better"
build, and nothing here is meant to replace sylverb's. It only *adds* on top, for anyone who
wants to try the experiments.

<p align="center">
  <img src="/img/clock-hero.jpg" width="520" alt="The Clock app running on a Game & Watch (Zelda model)" />
</p>

:::info Where the full documentation lives
These pages document **only what this fork adds or changes.** The hardware mod, installation,
controls, per-emulator notes for the stock systems and the FAQ live in the
[upstream README](https://github.com/sylverb/game-and-watch-retro-go-sd) and apply here unchanged.
:::

## Additional to the upstream firmware

There is no in-app "labs" switch — **the experimental firmware is simply a different firmware.**
Both this fork and upstream install the same way (`retro-go_update.bin` on the SD root) and share
your ROMs and SD layout, so you can move between them freely:

- For the **stable base**, flash sylverb's `retro-go_update.bin`.
- To try the **experimental additions**, flash this fork's `retro-go_update.bin`
  (`testbed-full-*` from the [releases page](https://github.com/jshsakura/game-and-watch-retro-go-sd/releases)).

Only savestates may differ between the two, so keep a backup.

## Install in one line

Flash the latest `retro-go_update.bin` from the
[releases page](https://github.com/jshsakura/game-and-watch-retro-go-sd/releases) — it carries the
matching SD payload (cores, homebrew overlays, BIOS logo) and installs it on first boot. The
hardware mod and one-time bootloader setup are the
[upstream steps](https://github.com/sylverb/game-and-watch-retro-go-sd).

:::warning Before you flash
Releases here are **test builds, not stable**.

- **Back up your SD card / saves first.** Some changes touch the SD read/write path and the
  savestate format; a bad build can corrupt or invalidate saves.
- Test builds may show on-screen debug overlays and may change without notice.
- Builds from **2026-07-15 on include boot-loop rescue**: two failed boots stop the third at a
  rescue screen instead of hanging dark. To recover a truly dead firmware, put a known-good
  `retro-go_update.bin` on the SD root and power on.
- For the stable, official project use
  [sylverb/game-and-watch-retro-go-sd](https://github.com/sylverb/game-and-watch-retro-go-sd).
:::

## Where to go next

- [Supported systems](./systems.md) — everything this fork adds or enables
- [Game Boy Advance](./game-boy-advance.md) — Pokémon at full speed
- [Super Metroid](./super-metroid.md)
- [Overclock & power](./overclock-and-power.md)
- [Devlog](/devlog) — the development journal
