---
slug: i18n-the-line-comment-that-ate-fifty-language-fields
title: 'i18n: the line comment that ate fifty language fields'
authors: [jshsakura]
tags: [i18n, fault]
image: /img/clock-hero.jpg
---

The build was green. The language-binary generator printed "0 missing." Every
language file loaded. And on the device, the welcome popup showed the wrong
text in the wrong box, in every language, past a certain point in the struct.
Fifty fields had quietly vanished from the generator's view. The build had not
noticed. Nothing had noticed.

{/* truncate */}

This is the story of how a `/*` hidden inside a `//` comment ate fifty fields
of a struct that is indexed by position, and how the entire toolchain smiled
at the result.

## The struct that is indexed by position

Every string the launcher can show lives in a `lang_t` struct — a flat list
of `const char *` fields, one per visible string:

```c
typedef struct {
    const char *s_welcome;
    const char *s_continue;
    const char *s_save_state;
    /* ... 277 fields ... */
    const char *s_volume;
} lang_t;
```

The launcher does not look up strings by name. It looks them up by **index**:
`lang->s_welcome` is field 0, `lang->s_continue` is field 1, and so on. At
runtime, the launcher loads a binary language file (`/lang/ko.bin`,
`/lang/en.bin`, etc.) straight into a `lang_t` struct as a flat array of
pointers. Field 0 of the binary is field 0 of the struct. The index *is* the
contract.

This is fast and cheap and it has one rule, which is painted on the wall in
the CLAUDE.md: **never insert or delete a field mid-struct.** Add only at the
end. A retired string is commented `RETIRED ... slot kept, unused` and left
in place. Because if you insert a field at position 50, every language file
built before the insertion now has its field 50 in field 51's slot, field 51
in field 52's slot, and every label past the gap shows the wrong text. The
binary loads to the end. It reads nonsense. And nothing fails — the file is
well-formed, the pointers are valid, the strings are real strings, they are
just in the wrong boxes.

That is the trap. Now the bug.

## The generator

The language binaries are built by a Python script, `gen_i18n_bin.py`, which
parses `rg_i18n_lang.h` (the C header that defines `lang_t`), extracts every
`const char *s_XXX;` field in order, looks up each one's translation in a
per-language dictionary, and writes the binary. The script strips comments
out of the header before parsing — both `//` line comments and `/* */`
block comments — so that commented-out fields don't confuse the parser.

The comment-stripper uses a regex. The regex for block comments is roughly
`re.sub(r'/\*.*?\*/', '', text, flags=re.DOTALL)`. It matches a `/*`,
everything up to the next `*/`, and removes it. Standard. Works fine.

Except.

## The landmine

`rg_i18n_lang.h` had this line, somewhere in the middle:

```c
    // against an older /lang/*.bin. Appended fields simply fall back to
```

It is a **line comment**. It is also a line comment that contains the
character sequence `/*` — inside the phrase "older /lang/*.bin." The `/*`
sits there harmlessly as long as no `*/` ever follows it anywhere in the
file. The line-comment stripper removes the whole line, the block-comment
stripper never sees the `/*`, everything is fine.

The line-comment stripper runs first. The block-comment stripper runs second.

But one day, someone added a block comment elsewhere in the file — a
normal `/* ... */` annotation, sixty lines below the line comment. And the
block-comment regex, scanning the already-line-stripped text, found the
`/*` that had been hiding inside the line comment — *which the line
stripper had already removed, but only if it ran first*.

The order of the two strippers had been swapped in a refactor. The block
stripper now ran first. It saw the `/*` inside the line comment, scanned
forward for the next `*/`, found it sixty lines later, and blanked
everything in between — the `/*`, the sixty lines of struct definitions,
and the `*/`. Fifty `const char *` fields vanished from the generator's
view. The struct, from the generator's perspective, went from 277 fields
to 227.

## Why nothing failed

The generator parsed 227 fields. It looked up each one's translation. It
wrote a binary with 227 entries. It printed `0 missing` — because it had
found a translation for every field it could see. The fifty fields it
couldn't see, it didn't ask about.

The build compiled the C header (which the compiler parses correctly —
the C preprocessor handles comments the right way, line comments first).
The struct in the binary was 227 entries. The struct in the C code was 277
entries. They disagreed. Nothing checked.

The launcher loaded the 227-entry binary into the 277-entry struct. Field
0 of the binary went into field 0 of the struct (correct). Field 1 into
field 1 (correct). This continued until field 49 — the last field before
the gap. Field 50 of the binary, which was *actually* the struct's field
100 (because the generator had skipped the fifty fields in the gap), went
into field 50 of the struct. Every field from 50 onward was now pointing
at the wrong string. The welcome popup showed the volume label. The
"continue" entry in the file menu showed the save-state prompt. The
settings page was a collage of misplaced text in every language.

And the build was green. The generator was happy. The binary loaded
without error. Nothing failed.

## The fix

Three changes, because one is not enough.

**One: strip line comments *first*.** Always. A `/*` inside a `//` comment
is not a block-comment opener, and the only way the block stripper learns
that is if the line stripper has already removed the line. The order is the
contract.

**Two: make the generator count fields the dumbest possible way.** After
parsing, the generator walks the header again with a separate, brain-dead
regex — `re.findall(r'const char \*s_\w+;', text)` — and counts the
matches. If that count disagrees with what it parsed, **the generator
refuses to run.** It does not print a warning. It does not continue. It
exits with an error listing every field it missed. A field the parser
cannot see is not a warning, it is a wrong binary.

**Three: defuse the landmine and post a guard.** Reword the comment so the
`/*` is no longer there (`older /lang/*.bin` → `older language binary`).
And add a check in `tests/run.sh`: grep the header for any `//` line that
contains `/*`, and fail if one appears. The next person who writes a
comment about path globs will hear about it at CI time, not at user-report
time.

Revert the stripper-order fix alone, and the guard fires by name, listing
all fifty fields. Revert the field-count guard alone, and the generator
quietly emits a 227-field binary again. Both have to be in place for the
test to mean anything.

## The cousin: the missing comma

While auditing the same header for this bug, I found a cousin. The font
list — an array of path string literals — was missing a comma between two
entries:

```c
const char *fonts[FONT_COUNT] = {
    "greybeard"
    "serif_bold",   /* <-- no comma before this → C string concatenation */
    /* ... */
};
```

C concatenates adjacent string literals separated by whitespace. So
`"greybeard" "serif_bold"` became `"greybeardserif_bold"` — one bogus path,
not two real ones. The array went from 9 entries to 8. Font index 5 opened
a file that does not exist. Indexes 7 and 8 read past the end of the array.
The Cyrillic font list was 8 entries, not 9, and the welcome popup's
Cyrillic glyphs were corrupted.

Fix: add the comma, and `_Static_assert` both font lists to `FONT_COUNT` so
the next missing comma is a build error, not a glyph-corruption report.

## The lesson

The i18n bug taught me one thing that has changed how I write every
generator since:

**A generator that emits a wrong output silently is worse than no generator
at all.** The whole point of generating the language binary from the header
is that the binary *cannot* disagree with the struct definition. The
moment the generator's view of the struct can diverge from the compiler's
view — because of a comment-stripping order, because of a regex that
doesn't handle an edge case, because of a missing comma that the language
silently paper- over — the generator becomes a machine for producing
plausible-looking wrong binaries. And the build stays green. And the user
sees nonsense on the screen and reports it as a glyph bug.

The defence is not a better regex. The defence is a **round-trip check**:
the generator must, after parsing, count what it parsed by an entirely
different method, and refuse to run if the two counts disagree. The
compiler's view of the struct is the truth. The generator's view is a
guess. The guess must prove itself against the truth every time, or it is
just a faster way to ship the wrong thing.

The language files are correct now. Every field is in its slot. The
welcome popup shows the welcome text. The font list is nine entries. And
the generator, the moment it cannot see a field, says so by name and stops
— because the only wrong answer it is allowed to give is no answer at all.
