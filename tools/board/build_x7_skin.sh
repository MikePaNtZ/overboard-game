#!/bin/bash
# Rebuild the X7 concept board skin (-ObBoardSkin=x7) from the hardware track's exterior model.
# usage: tools/board/build_x7_skin.sh [path/to/overboard_x7_exterior_dark.glb]
# The model is CONCEPT proxy geometry from overboard-viz-kit (overboard-91), not CAD.
# 1. Blender splits it into the static frame and the spinning motor, both with the axle as origin,
#    and prints the pad tops (the rider's deck height, RenderDeckTopCm in BoardActor).
# 2. The editor imports both to /Game/ThirdParty/X7 (gitignored).
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-/Users/mike/projects/overboard-viz-kit/out/kit-guide/models/overboard_x7_exterior_dark.glb}"
mkdir -p /tmp/ob-board
blender --background --factory-startup --python "$HERE/tools/board/split_x7.py" -- "$SRC" /tmp/ob-board 2>&1 \
  | grep -E "motor meshes|PAD|Error|Traceback"
"$HERE/tools/city/ue.sh" "$HERE/tools/board/import_x7.py"
cat /tmp/ob-board/import.txt | grep StaticMesh
