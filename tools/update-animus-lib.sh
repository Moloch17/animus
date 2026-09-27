#!/usr/bin/env bash
# Refresh the bundled animus-lib (animus-lib/src/runtime) from an Animus Forge checkout.
#
#     tools/update-animus-lib.sh [forge checkout]      # default: the forge this module sits in (../..), if it is one
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
# After a refresh: build against a stock core (the syntax is not enough: a forge-only call only fails at link), and
# check the conf template documents every tuning key (this script does that last).
set -euo pipefail

cd "$(dirname "$0")/.."
forge="${1:-../..}"
core="$forge/src/server/game/Animus"
if [[ ! -d "$core/Scenario/Curriculum" ]]; then
  echo "No forge core at $forge (looked for $core/Scenario/Curriculum); pass a forge checkout." >&2
  exit 1
fi

bundle=animus-lib/src/runtime
owned='^(animus_lib_loader\.cpp|Core/.*|Model/.*|Bot/BotFactory\.(h|cpp))$'

copied=0
kept=()
while IFS= read -r file; do
  if [[ "$file" =~ $owned ]]; then
    kept+=("$file")
  elif [[ -f "$core/$file" ]]; then
    cp "$core/$file" "$bundle/$file"
    copied=$((copied + 1))
  else
    kept+=("$file (the forge has no such file any more)")
  fi
done < <(cd "$bundle" && find . -type f \( -name '*.cpp' -o -name '*.h' \) | sed 's|^\./||' | sort)
echo "Copied $copied files from $core."
printf '  kept the module'"'"'s own: %s\n' "${kept[@]}"

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

revision="$(git -C "$forge" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "$revision" > animus-lib/FORGE_REVISION
echo "Bundled animus-lib is now the forge's runtime at $revision (animus-lib/FORGE_REVISION)."

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
exit "$missing"
