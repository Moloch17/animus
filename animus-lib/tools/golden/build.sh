#!/usr/bin/env bash
# Build golden_check against Model/MlpPolicy.cpp alone, in a throwaway container (the forge's dev image has g++ 13),
# with the module's forward-pass flags (AnimusLibTuneForwardPass) or, with --plain, -O2 alone; and run it on the
# golden directory given.
#
#     tools/golden/build.sh <golden dir> <model> [<model> ...] [--plain] [--time N] [--tolerance T]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
lib="$(cd "$here/../.." && pwd)"
golden="$(cd "$1" && pwd)"
shift
flags="-O3 -fassociative-math -fno-signed-zeros -fno-trapping-math"
args=()
for arg in "$@"; do
  if [[ "$arg" == "--plain" ]]; then flags="-O2"; else args+=("$arg"); fi
done
image="${GOLDEN_IMAGE:-ac-animus-forge-dev-server:latest}"
docker run --rm -v "$lib:/animus-lib:ro" -v "$golden:/golden:ro" --entrypoint bash "$image" -c "
  set -e
  g++ -std=c++20 $flags -I/animus-lib/tools/golden/stub -I/animus-lib/src/runtime/Model \
    /animus-lib/tools/golden/golden_check.cpp /animus-lib/src/runtime/Model/MlpPolicy.cpp -o /tmp/golden_check
  /tmp/golden_check /golden ${args[*]}
"
