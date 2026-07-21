---
slug: six-overflows-in-a-toy-launcher
title: 'Six overflows in a toy launcher'
authors: [jshsakura]
tags: [fault, security, savestate]
image: /img/clock-hero.jpg
---

It is a Game & Watch. It plays ROMs off an SD card. It is not networked. It
does not accept untrusted input. There is nothing to attack.

That is the lie I told myself for a year, until I ran an adversarial pass
over the launcher's C and it came back with **six** buffer overflows, three
of them hardfault-class on a long filename. The SD card is an attack surface.
The filename is user input. "Embedded" is not a pass on security review.

{/* truncate */}

## The pattern that did all the damage

Three of the six bugs were the same bug. The shape is:

```c
strncpy(dst, src, strlen(src) - something);
```

Read it carefully. The third argument to `strncpy` is **the destination's
capacity**, not the source's length. Passing `strlen(src)` (or a derivative
of it) says "copy as many bytes as the source has." If the source is longer
than `dst`, you overflow `dst`. The function does not protect you — you told
it the wrong capacity, and it believed you.

That is the whole footgun. `strncpy(dst, src, sizeof(dst))` is safe.
`strncpy(dst, src, strlen(src))` is `strcpy` with a misleading name. Three
places in the launcher had typed the second shape.

### AR-01: the file object that ate its own metadata

`emulator_build_file_object` parsed an SD path into a struct:

```c
strncpy(file->folder, path, strlen(path) - strlen(name) - 1);
strncpy(file->name,   name, strlen(name) - strlen(ext) - 1);
```

`file->folder` is **32 bytes**. The path can be 255. A ROM nested in
`/roms/nes/very_long_subfolder_name/game.nes` overflows `folder` straight
into the adjacent struct members — `size`, `crc_offset`, `checksum`,
`emulator` — which then hold fragments of the folder string interpreted as
integers and pointers. Half the time that is a Hardfault; the other half it
is a corrupted checksum that silently breaks resume. And `strncpy` does not
null-terminate when the source fills the buffer, so the overflow reads as a
run-on string into the next field.

The fix is what the third argument should always have been:

```c
size_t folder_len = strlen(path) - strlen(name) - 1;
if (folder_len >= sizeof(file->folder))
    folder_len = sizeof(file->folder) - 1;
strncpy(file->folder, path, folder_len);
file->folder[folder_len] = '\0';
```

Clamp to the destination, terminate by hand. Every `strncpy` in the file now
looks like that.

### AR-02: favorites, the 168-byte path, and the 128-byte slot

`favorites_save` did:

```c
char *buffer = calloc(favorites_count, 128);
for (i ...) strcat(buffer, favorites[i].path); strcat(buffer, "\n");
```

`favorite_t.path` is **168 bytes**. The allocator gave each favorite 128.
Three long paths in a row and `strcat` walks off the end of `buffer` into
whatever the heap put after it. Worse: `favorites_count == 0` makes
`calloc(0, 128)`, which is allowed to return `NULL`, which then goes to
`odroid_settings_string_set(..., NULL)` — a null deref on an empty favorites
list.

The fix sizes the buffer to the real worst case (sum of `strlen(path) + 1`
across all live entries, plus a null terminator), and returns early on
allocation failure.

### AR-03: the assert that was commented out

```c
const char *rg_dirname(const char *path) {
    static char buffer[100];
    const char *basename = strrchr(path, '/');
    ptrdiff_t length = basename - path;
    // ...
//  RG_ASSERT(length < 100, "to do: use heap");
    strncpy(buffer, path, length);
    buffer[length] = 0;
    return buffer;
}
```

A 100-byte static buffer, a path longer than 99 bytes, and an assert that
someone had typed and then commented out because the fix was "to do." The
comment was the bug. `buffer[length] = 0` writes past the end of `buffer`,
into adjacent `.bss`, every call. The assert would have caught it on the
first long path; without it, the function silently corrupts globals until
something unrelated breaks.

I uncommented the assert and sized the buffer to `RG_PATH_MAX`. The "to do"
became the fix.

## The two that were not strncpy

### AR-04: the byte-swap that leaked stack into flash

`circular_flash_write` swaps byte pairs before writing ROM data to QSPI
flash:

```c
for (size_t i = 0; i < bytes_read; i += 2) {
    uint8_t temp = buffer[i];
    buffer[i] = buffer[i + 1];
    buffer[i + 1] = temp;
}
```

If `bytes_read` is odd — an odd-sized ROM, or a short read at end of file —
the last iteration reads `buffer[i+1]` past the end of the valid data. What
it reads is **uninitialised stack**. What it writes is that stack byte,
swapped into your ROM, persisted to external flash. So a hardware fault
becomes a data-corruption bug *and* an information leak: random stack bytes
baked into the flash image, readable later.

The fix masks the loop bound down to even: `size_t limit = bytes_read & ~1;`
The odd trailing byte is left untouched.

### AR-05: the root path that became the current directory

```c
char dir[RG_PATH_MAX];
strncpy(dir, path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = '\0';
char *last_slash = strrchr(dir, '/');
if (!last_slash) return false;
*last_slash = '\0';
```

If `path` is `/game.gbc` (a ROM at the SD root), `last_slash` points at index
0. `*last_slash = '\0'` truncates `dir` to the empty string `""`. FatFs
treats `""` as **the current working directory** — which is not necessarily
the root. The previous/next-file browser then walks whatever folder the CWD
happens to be in, and the feature looks broken on root-level ROMs.

One line:

```c
if (dir[0] == '\0') strcpy(dir, "/");
```

### AR-06: the backup domain left unlocked

`GW_RTC_RestoreIfLost` calls `HAL_PWR_EnableBkUpAccess()` to restore the
clock from an SD snapshot when the backup domain has lost power. It never
calls `HAL_PWR_DisableBkUpAccess()`. The backup registers — including the
boot-counter that the
[boot rescue screen](/devlog/boot-rescue-when-a-hung-boot-was-a-dead-battery)
depends on — are left writable for the whole session. A wild pointer store,
a brownout, a peripheral mis-behaviour can all now scribble the RTC region.

Closing the lock after the write is one line. Low severity, but the kind of
thing you fix because it is free.

## The lesson I actually learned

Two lessons.

The first: **`strncpy(dst, src, strlen(src))` is `strcpy` with a lie for a
name.** Every call site that derives the bound from the source length is a
buffer overflow waiting for a long enough input. The bound is the
destination's capacity. Always. If you find yourself typing `strlen` in the
third argument, stop and read it again.

The second: **the assert you commented out is the bug you are about to
ship.** AR-03 had the fix *in the source*, behind a `// to do: use heap`
comment. It sat there through a release. The comment was honest — the
author knew the static buffer was wrong, knew the fix was to allocate, and
shipped it anyway because the assert would have fired on a path nobody
tested. Six months later, somebody tested it.

A toy launcher is still a C program reading filenames off untrusted media.
The SD card does not know it is inside a Game & Watch. The
[adversarial review](https://github.com/jshsakura/game-and-watch-retro-go-sd/blob/perf/32x-histogram/ADVERSARIAL_REVIEW.md)
found six real faults in an afternoon — three of them the same fault, typed
three times. The build had been green for a year. Green is what the tests
tell you, not what the program is.
