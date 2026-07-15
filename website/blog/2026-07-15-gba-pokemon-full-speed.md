---
slug: gba-pokemon-full-speed
title: "GBA Pokémon runs full speed — and the music was a third of the game"
authors: [jshsakura]
tags: [gba, performance, audio]
image: /img/clock-hero.jpg
---

Two months in, and I still can't put it down: **Pokémon Ruby and Emerald now run at full speed on
the Game & Watch**, sound and all. This first devlog entry is the short version of how a 340 MHz
microcontroller with 724 KB of RAM ended up running a Game Boy Advance.

{/* truncate */}

## The emulator doesn't fit, so it isn't all in one place

gpSP is 853 KB of code and data; the per-core RAM pool is 724 KB. It only works because the
emulator is split four ways — the hot interpreter in ITCM, the renderer and cold code executed
straight from external flash (XIP), big buffers on the slower AHB SRAM, and the rest in the pool.
The 16 MB Pokémon ROM never enters RAM at all: it's cached to flash and read where it lies.

Details: [Game Boy Advance](/docs/game-boy-advance).

## The surprise: a third of the "game" was the music

Almost every commercial GBA cart ships Nintendo's **M4A** sound library, and that library
**software-mixes its audio on the guest CPU** — every frame, whether anything is happening or not.
Measured across six engine variants, **27–60% of all guest instructions were the mixer**, not the
game.

So the firmware recognises the mixer by signature and runs a **native transcription** of it instead
of interpreting it instruction by instruction — proven bit-identical against 40,000+ blocks and
13,000+ frames of lockstep. The payoff: Zelda: The Minish Cap −60%, Emerald −35%, FFTA −27% guest
instructions. That's the difference between a slideshow and a playable game.

## Honest status

- Ruby and Emerald hold 60 fps; heavier titles vary, and FFTA still sits just under.
- Audio got another pass since the video clip I first shared — the leftover harshness is cleaned up.
- One intermittent audio tick is still under investigation.

This is an experimental fork and a bit of a mess, but it's been a genuinely fun two months. If any
of it is useful, I hope it makes its way upstream.
