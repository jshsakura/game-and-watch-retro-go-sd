#!/usr/bin/env bash
# Refuse to package a release whose sd_content/ holds output from an older build.
#
# release rolls the tar from sd_content/ wholesale:
#     tar -C sd_content $(find sd_content -mindepth 1 -maxdepth 1 ...)
# so every file sitting in that directory ships, whether this build produced it
# or not. .gitignore does not apply and no git-based audit can see it. Two
# releases have already been wrong for exactly this reason:
#   0727  305 CC BY-NC sprite files staged for testing rode into a public tar.
#   0727  a removed core's payload (cores/32x.bin + cores/32x.xip, ~1 MB) was
#         still on disk from a build made before the core was deleted, and the
#         next tar carried a core the firmware can no longer even dispatch.
#
# create_sd_data rewrites every file it owns each time the ELF relinks, so the
# rule is simply: under the directories the build owns, nothing may be older
# than the ELF. Anything that is, this firmware does not produce.
#
# Scoped to cores/ and roms/homebrew/ on purpose -- bios/*.rom and the MSX blobs
# are repo content that is copied, not generated, and are legitimately older.
set -u

ELF="${1:-}"
if [ -z "$ELF" ]; then
    echo "usage: check_sd_content_fresh.sh <path/to/gw_retro_go.elf>" >&2
    exit 2
fi
if [ ! -f "$ELF" ]; then
    # No ELF means the caller is not in a position to judge freshness. Say so and
    # get out of the way: a safety net must not be the thing that breaks a build.
    echo "sd-content-fresh: SKIPPED (no ELF at $ELF) -- staleness NOT verified"
    exit 0
fi
if [ ! -d sd_content ]; then
    echo "sd-content-fresh: SKIPPED (no sd_content/) -- staleness NOT verified"
    exit 0
fi

stale=""
for dir in sd_content/cores sd_content/roms/homebrew; do
    [ -d "$dir" ] || continue
    while IFS= read -r f; do
        stale="$stale  $f"$'\n'
    done < <(find "$dir" -type f ! -newer "$ELF" ! -samefile "$ELF" 2>/dev/null | sort)
done

if [ -n "$stale" ]; then
    printf 'sd-content-fresh: FAILED -- sd_content/ holds output older than %s:\n' "$ELF"
    printf '%s' "$stale"
    echo   "These predate this build, so create_sd_data did not write them: they belong"
    echo   "to a core or feature this firmware no longer has, and the release tar is"
    echo   "built from the directory, so they WOULD ship. Delete them and re-run:"
    printf '%s' "$stale" | sed 's/^  /  rm /'
    exit 1
fi

echo "sd-content-fresh: OK (every build output under sd_content/ is from this link)"
