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
