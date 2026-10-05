#!/usr/bin/env bash
# Refresh the bundled animus-lib (animus-lib/src/runtime) from an Animus Forge checkout.
#
#     tools/update-animus-lib.sh [forge checkout]          # default: the forge this module sits in (../..)
#     tools/update-animus-lib.sh --check [forge checkout]  # copy nothing: only prove the bundle is the forge's
#     tools/update-animus-lib.sh --only Movement [...]     # refresh (or --check) one directory of the bundle only
#
# To bundle a commit rather than a working tree (the forge branch has work in progress), export it first:
#     git -C <forge> archive <commit> src/server/game/Animus | tar -x -C /tmp/forge-<commit>
#     FORGE_REV=<commit> tools/update-animus-lib.sh --only Movement /tmp/forge-<commit>
#
# The forge folded animus-lib into its core (src/server/game/Animus), so that is where the curriculum lives now: the
# blocks, the layout and its manifest, the characters, the stages and the tuning. A model is refused unless the
# manifest this module builds is exactly the one it was exported with, so the bundle must be refreshed from the forge
# revision the models were trained on, together with the models.
#
# Every file of the bundle that the forge also has is copied from it as it is. The module keeps its own:
#   animus_lib_loader.cpp, Core/ (CoreHooks), Model/ (the .amdl reader and the model library), and Bot/BotFactory.*
#   (the forge's creates sim-session bots through forge-only core calls; this one places companions beside players)
# and Scenario/Curriculum/WarmCaches.cpp is left out (it warms the training half's tables; GearBuilder only needs the
# header). A file the forge has and the bundle does not is not added unless something includes it: the script lists
# every include the bundle can no longer resolve, and where the forge has it.
#
# Whole directories are taken as the forge has them (`whole`): every file in them is copied, new ones included, and a
# file the forge dropped is dropped, except the forge-only ones (`forge_only`). Movement/ is the player controller and
# the client logic both sides share (the physics, the report cadence, the server's orders and the acks); its
# PlayerLink is the forge's server link (ClientMovement, a forge-core refactor of the movement handler), and the
# realm's is the module's own (src/Client/CompanionClient).
#
# Last, every bundled file the forge also has is compared byte for byte with it (cmp): a difference fails the script,
# so a shared file cannot drift. `--check` does only that.
#
# After a refresh: build against a stock core (the syntax is not enough: a forge-only call only fails at link), and
# check the conf template documents every tuning key (this script does that last).
set -euo pipefail

cd "$(dirname "$0")/.."
check_only=0
only=''
while [[ "${1:-}" == --* ]]; do
  case "$1" in
    --check) check_only=1; shift ;;
    --only) only="${2:?--only needs a directory of the bundle, e.g. Movement}"; only="${only%/}/"; shift 2 ;;
    *) echo "Unknown option $1" >&2; exit 1 ;;
  esac
done
# A path inside the refresh: everything, or the --only directory's.
inside() { [[ -z "$only" || "$1" == "$only"* ]]; }
forge="${1:-../..}"
core="$forge/src/server/game/Animus"
if [[ ! -d "$core/Scenario/Curriculum" ]]; then
  echo "No forge core at $forge (looked for $core/Scenario/Curriculum); pass a forge checkout." >&2
  exit 1
fi

bundle=animus-lib/src/runtime
owned='^(animus_lib_loader\.cpp|Core/.*|Model/.*|Bot/BotFactory\.(h|cpp))$'
whole=(Movement)
forge_only='^Movement/PlayerLink\.(h|cpp)$'

# Every bundled file the forge has must be the forge's, byte for byte (the module's own and the forge-only excepted).
compare() {
  local differ=0 same=0 file
  while IFS= read -r file; do
    [[ "$file" =~ $owned || "$file" =~ $forge_only ]] && continue
    inside "$file" || continue
    [[ -f "$core/$file" ]] || continue
    if cmp -s "$core/$file" "$bundle/$file"; then
      same=$((same + 1))
    else
      echo "  differs from the forge: $file" >&2
      differ=1
    fi
  done < <(cd "$bundle" && find . -type f \( -name '*.cpp' -o -name '*.h' \) | sed 's|^\./||' | sort)
  echo "cmp: $same bundled files identical to $core$( ((differ)) && echo ', some differ')."
  return "$differ"
}

if ((check_only)); then
  compare
  exit $?
fi

# The whole directories first, so the loop below finds their new files in the bundle and copies them like the rest.
for dir in "${whole[@]}"; do
  inside "$dir/" || continue
  mkdir -p "$bundle/$dir"
  while IFS= read -r file; do
    [[ "$dir/$file" =~ $forge_only ]] && continue
    mkdir -p "$(dirname "$bundle/$dir/$file")"
    cp "$core/$dir/$file" "$bundle/$dir/$file"
  done < <(cd "$core/$dir" && find . -type f \( -name '*.cpp' -o -name '*.h' \) | sed 's|^\./||' | sort)
  while IFS= read -r file; do
    if [[ ! -f "$core/$dir/$file" || "$dir/$file" =~ $forge_only ]]; then
      rm "$bundle/$dir/$file"
      echo "  dropped $dir/$file (the forge has no such file, or it is forge-only)"
    fi
  done < <(cd "$bundle/$dir" && find . -type f | sed 's|^\./||' | sort)
done

copied=0
kept=()
while IFS= read -r file; do
  if ! inside "$file"; then
    continue
  elif [[ "$file" =~ $owned ]]; then
    kept+=("$file")
  elif [[ -f "$core/$file" ]]; then
    cp "$core/$file" "$bundle/$file"
    copied=$((copied + 1))
  else
    kept+=("$file (the forge has no such file any more)")
  fi
done < <(cd "$bundle" && find . -type f \( -name '*.cpp' -o -name '*.h' \) | sed 's|^\./||' | sort)
echo "Copied $copied files from $core."
((${#kept[@]})) && printf '  kept the module'"'"'s own: %s\n' "${kept[@]}"

# Includes the bundle cannot resolve: its own headers, then the core's (a stock core has everything but the forge's).
missing=0
while IFS= read -r header; do
  if [[ -z "$(find "$bundle" -name "$header" -print -quit)" ]]; then
    found="$(cd "$core" && find . -name "$header" | sed 's|^\./||' | head -1)"
    if [[ -n "$found" ]]; then
      echo "  unresolved include $header: the forge has $found (copy it, and its .cpp if the bundle needs it)" >&2
      missing=1
    fi
  fi
done < <(grep -rhoE '#include "[^"]+\.h"' "$bundle" | sed 's/#include "//;s/"//' | sort -u)

# A checkout's own revision; for an export of a commit (git archive, which is how a forge branch with uncommitted
# work is bundled at a commit), FORGE_REV names it.
revision="${FORGE_REV:-$(git -C "$forge" rev-parse --short HEAD 2>/dev/null || echo unknown)}"
if [[ -n "$only" ]]; then
  # The first line is the revision of the whole bundle; a directory refreshed on its own gets a line of its own.
  { head -1 animus-lib/FORGE_REVISION; tail -n +2 animus-lib/FORGE_REVISION | grep -v "^$only " || true;
    echo "$only $revision"; } > animus-lib/FORGE_REVISION.new
  mv animus-lib/FORGE_REVISION.new animus-lib/FORGE_REVISION
  echo "Bundled ${only%/} is now the forge's at $revision (animus-lib/FORGE_REVISION)."
else
  echo "$revision" > animus-lib/FORGE_REVISION
  echo "Bundled animus-lib is now the forge's runtime at $revision (animus-lib/FORGE_REVISION)."
fi

# The lib declares every curriculum tuning key once (CurriculumTuning::Visit) and this module documents them
# again in its conf template. Updating the lib is when they drift, so this is where it is checked: a key the
# sim reads and the template does not mention is invisible -- Load asks for each with a default and no warning,
# so it silently keeps its compiled-in value and nobody can find out it exists. The forge's worldserver.conf.dist
# documents every one as AnimusForge.Curriculum.*.
tuning="$bundle/Scenario/Curriculum/CurriculumTuning.h"
conf="conf/mod_animus.conf.dist"
undocumented=$(comm -23 \
  <(grep -oE 'f\("[A-Za-z0-9.]+"' "$tuning" | sed 's/f("//;s/"//' | sort -u) \
  <(grep -oE "^[A-Za-z]+\.Curriculum\.[A-Za-z0-9.]+" "$conf" | sed 's/^[^.]*\.Curriculum\.//' | sort -u))
if [[ -n "$undocumented" ]]; then
  echo
  echo "These tuning keys are read by the sim but not documented in $conf:" >&2
  echo "$undocumented" | sed 's/^/  /' >&2
  echo "Copy their blocks from the forge's worldserver.conf.dist (AnimusForge.Curriculum.* -> Animus.Curriculum.*)." >&2
  missing=1
else
  echo "Every tuning key the lib reads is documented in $conf."
fi

compare || missing=1
exit "$missing"
