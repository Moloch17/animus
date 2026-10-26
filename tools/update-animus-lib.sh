#!/usr/bin/env bash
# Refresh the bundled runtime (animus-lib/src/runtime) from an Animus Forge checkout.
#
#     tools/update-animus-lib.sh [forge checkout]          # default: the forge this module sits in (../..)
#     tools/update-animus-lib.sh --check [forge checkout]  # copy nothing: only prove the bundle is the forge's
#
# To bundle a commit rather than a working tree, export it first:
#     git -C <forge> archive <commit> src/server/game/Animus/Runtime | tar -x -C /tmp/forge-<commit>
#     FORGE_REV=<commit> tools/update-animus-lib.sh /tmp/forge-<commit>
#
# The forge keeps everything a trained model needs at run time in src/server/game/Animus/Runtime (2026-10-08): the
# blocks, the layout and its manifest, the characters, the stages, the camera (Vision/), the player controller and the
# bots; nothing there includes the training half (apps/forge/tools/runtime_graph_check.py holds that line). The bundle
# is that tree, mirrored: every file copied as the forge has it, new ones added, and a file the forge no longer has
# removed. A model is refused unless the manifest this module builds is exactly the one it was exported with, so the
# bundle must be refreshed from the revision the models were trained on, together with the models.
#
# The module keeps its own (never copied, never removed): animus_lib_loader.cpp, Core/ (CoreHooks: what a stock core
# has in place of the forge's hooks), Model/ (the .amdl reader and the forward pass: the forge has no inference),
# Bot/BotFactory.* and Bot/BotSlot.* (the forge's create sim-session bots through forge-only calls; these place bots
# beside players on a realm). Movement/PlayerLink.* is the forge's server link and is not bundled: the realm's is the
# module's own (src/Client/CompanionClient).
#
# Last, every bundled file is compared byte for byte with the forge's (cmp): a difference fails the script, so a
# shared file cannot drift. `--check` does only that. After a refresh: build against a stock core (the syntax is not
# enough: a forge-only call only fails at link).
set -euo pipefail
cd "$(dirname "$0")/.."

check_only=0
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --check) check_only=1; shift ;;
    *) echo "Unknown option $1" >&2; exit 1 ;;
  esac
done

forge="${1:-../..}"
core="$forge/src/server/game/Animus/Runtime"
if [[ ! -d "$core/Scenario/Curriculum" ]]; then
  echo "No forge runtime at $forge (looked for $core/Scenario/Curriculum); pass a forge checkout." >&2
  exit 1
fi
bundle=animus-lib/src/runtime
owned='^(animus_lib_loader\.cpp|Core/.*|Model/.*|Bot/BotFactory\.(h|cpp)|Bot/BotSlot\.(h|cpp))$'
forge_only='^Movement/PlayerLink\.(h|cpp)$'

forge_files() { (cd "$core" && find . -type f \( -name '*.cpp' -o -name '*.h' \) | sed 's|^\./||' | sort); }
bundle_files() { (cd "$bundle" && find . -type f \( -name '*.cpp' -o -name '*.h' \) | sed 's|^\./||' | sort); }

# Every bundled file the forge has must be the forge's, byte for byte; and the forge's must all be bundled.
compare() {
  local differ=0 same=0 file
  while IFS= read -r file; do
    [[ "$file" =~ $owned || "$file" =~ $forge_only ]] && continue
    if [[ ! -f "$bundle/$file" ]]; then
      echo "  missing from the bundle: $file" >&2
      differ=1
    elif cmp -s "$core/$file" "$bundle/$file"; then
      same=$((same + 1))
    else
      echo "  differs from the forge: $file" >&2
      differ=1
    fi
  done < <(forge_files)
  while IFS= read -r file; do
    [[ "$file" =~ $owned ]] && continue
    if [[ ! -f "$core/$file" || "$file" =~ $forge_only ]]; then
      echo "  bundled but not the forge's runtime: $file" >&2
      differ=1
    fi
  done < <(bundle_files)
  echo "cmp: $same bundled files identical to $core$( ((differ)) && echo ', and differences (above)')."
  return "$differ"
}

if ((check_only)); then
  compare
  exit $?
fi

# Mirror: copy every forge file in, then drop every bundled file the forge does not have (the module's own aside).
copied=0
while IFS= read -r file; do
  [[ "$file" =~ $owned || "$file" =~ $forge_only ]] && continue
  mkdir -p "$(dirname "$bundle/$file")"
  cp "$core/$file" "$bundle/$file"
  copied=$((copied + 1))
done < <(forge_files)
dropped=0
while IFS= read -r file; do
  [[ "$file" =~ $owned ]] && continue
  if [[ ! -f "$core/$file" || "$file" =~ $forge_only ]]; then
    rm "$bundle/$file"
    dropped=$((dropped + 1))
  fi
done < <(bundle_files)
find "$bundle" -type d -empty -delete
echo "Copied $copied files from $core; dropped $dropped the forge no longer has."

# Includes the bundle cannot resolve within itself: either the core's (fine on a stock core) or a forge file outside
# the runtime, which the runtime must not need.
missing=0
while IFS= read -r header; do
  if [[ -z "$(find "$bundle" -name "$header" -print -quit)" ]]; then
    found="$(cd "$forge/src/server/game/Animus" && find . -name "$header" | sed 's|^\./||' | head -1)"
    if [[ -n "$found" ]]; then
      echo "  unresolved include $header: the forge has it outside its runtime ($found)" >&2
      missing=1
    fi
  fi
done < <(grep -rhoE '#include "[^"]+\.h"' "$bundle" | sed 's/#include "//;s/"//' | sort -u)

revision="${FORGE_REV:-$(git -C "$forge" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
echo "$revision" > animus-lib/FORGE_REVISION
echo "Bundled runtime is now the forge's at $revision (animus-lib/FORGE_REVISION)."

compare || missing=1
exit "$missing"
