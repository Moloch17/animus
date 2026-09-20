#!/usr/bin/env bash
# Update the bundled animus-lib (animus-lib/, a git subtree) to a revision of https://github.com/Moloch17/animus-lib.
#
#     tools/update-animus-lib.sh [ref]      # default: master
#
# Commit or stash your changes first: git subtree needs a clean tree. It commits the update itself.
set -euo pipefail

cd "$(dirname "$0")/.."
ref="${1:-master}"
url="https://github.com/Moloch17/animus-lib.git"

if [[ -n "$(git status --porcelain)" ]]; then
  echo "The working tree has changes; commit or stash them first." >&2
  exit 1
fi

git subtree pull --prefix=animus-lib "$url" "$ref" --squash -m "Update the bundled animus-lib to $ref"
git log -1 --format="Bundled animus-lib is now at: %s" -- animus-lib

# The lib declares every curriculum tuning key once (CurriculumTuning::Visit) and this module documents them
# again in its conf template. Updating the lib is when they drift, so this is where it is checked: a key the
# sim reads and the template does not mention is invisible -- Load asks for each with a default and no warning,
# so it silently keeps its compiled-in value and nobody can find out it exists.
tuning="animus-lib/src/Scenario/Curriculum/CurriculumTuning.h"
conf="conf/mod_animus.conf.dist"
if [[ -f "$tuning" && -f "$conf" ]]; then
  missing=$(comm -23 \
    <(grep -oE 'f\("[A-Za-z0-9.]+"' "$tuning" | sed 's/f("//;s/"//' | sort -u) \
    <(grep -oE "^[A-Za-z]+\.Curriculum\.[A-Za-z0-9.]+" "$conf" | sed 's/^[^.]*\.Curriculum\.//' | sort -u))
  if [[ -n "$missing" ]]; then
    echo
    echo "These tuning keys are read by the sim but not documented in $conf:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    echo "Add them, with a comment saying what they do, before committing." >&2
    exit 1
  fi
  echo "Every tuning key the lib reads is documented in $conf."
fi

