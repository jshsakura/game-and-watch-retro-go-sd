#!/bin/bash -e

if [ -z $1 ]; then
    echo "Usage: $(basename $0) <gittag.h>"
    exit 1
fi

gittagfile="$1"

TMPFILE=$(mktemp build/gittag.XXXXXX)
if [[ ! -e $TMPFILE ]]; then
    echo "Can't create tempfile!"
    exit 1
fi

# GIT_TAG_OVERRIDE lets the caller supply the tag when git cannot answer here.
# That is the normal case in the container: the bind mount is the worktree, but
# a worktree's .git is a file pointing into the parent repo, which is not
# mounted -- so every worktree build was stamping NOTAG, and a BSOD from one
# could not be told from a BSOD from any other.
GITTAG="${GIT_TAG_OVERRIDE:-}"
if [ -z "$GITTAG" ]; then
    GITTAG=$(git describe --tags --dirty=+ 2> /dev/null || echo "NOTAG")
fi

echo -e "#ifndef GIT_TAG\n#define GIT_TAG \"Retro-Go SD "${GITTAG}"\"\n#endif" > "${TMPFILE}"

if ! diff -q ${TMPFILE} ${gittagfile} > /dev/null 2> /dev/null; then
    echo "Updating git tag file ${gittagfile}"
    cp -f "${TMPFILE}" "${gittagfile}"
fi

rm -f "${TMPFILE}"
