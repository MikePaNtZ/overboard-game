#!/bin/bash
# Build OB_Embarcadero (Level 2) from the embarcadero course, end to end. One command, re-runnable:
#
#   tools/levels/embarcadero/build_embarcadero.sh [course_dir]
#
# Steps: gen_embarcadero.py (numpy + OSM: ground, corridor, kerbs, rails, water, buildings, marks,
# dressing) -> build_embarcadero.py in the full editor (meshes, materials, props, look, the map) ->
# verify_embarcadero.py (the drawn corridor vs the course height) -> stills_embarcadero.py + MRQ ->
# the terrain declaration + the regenerated verification header. Work files: /tmp/ob-levels-sf/.
set -eu
HERE="$(cd "$(dirname "$0")/../../.." && pwd)"
PY="${PY:-$HOME/.venvs/ob-levels/bin/python}"
COURSE="${1:-$HOME/projects/overboard-viz/out/carve-lab/data/courses/embarcadero}"
COURSE="$(cd "$COURSE" && pwd)"
SRC="$HOME/projects/overboard-viz/out/carve-lab/data/sources/embarcadero"
WORK="${OB_LEVELS_WORK:-/tmp/ob-levels-sf}"
DATA="$WORK/data"
export OB_LEVELS_WORK="$WORK" OB_SF_DATA="$DATA" OB_COURSE_DIR="$COURSE" OB_BUILD_LOG="$WORK/build.txt"
export OB_FRAMES_DIR="$WORK/frames" OB_LEVELS_DATA="$SRC"
mkdir -p "$WORK" "$DATA"

echo "1/5 course + OSM -> $DATA"
"$PY" "$HERE/tools/levels/embarcadero/gen_embarcadero.py" "$COURSE" "$DATA" | tail -10

echo "2/5 level (editor, offscreen; several minutes)"
rm -rf "$HERE/Content/Maps/OB_Embarcadero.umap" "$HERE/Content/Maps/OB_Embarcadero_BuiltData.uasset" \
  "$HERE/Content/Embarcadero/Meshes" "$HERE/Content/Embarcadero/Materials"
"$HERE/tools/levels/ue.sh" "$HERE/tools/levels/embarcadero/build_embarcadero.py" \
  || { tail -25 "$WORK/build.txt"; exit 1; }
grep -E "COMPILE ERRORS|MISSING|FAIL" "$WORK/build.txt" && echo "(see $WORK/build.txt)" || true

echo "3/5 verify (numpy: the drawn corridor vs the course height)"
"$PY" "$HERE/tools/levels/embarcadero/verify_embarcadero.py" | tail -3

echo "4/5 stills (editor builds the camera sequence, then MRQ renders offscreen)"
"$HERE/tools/levels/ue.sh" "$HERE/tools/levels/embarcadero/stills_embarcadero.py" \
  || { tail -15 "$WORK/editor_stills_embarcadero.log"; true; }
OB_MAP=/Game/Maps/OB_Embarcadero OB_SEQ=/Game/Embarcadero/Cinematics/SEQ_Embarcadero \
  OB_CINE=/Game/Embarcadero/Cinematics OB_REPLAY=/dev/null OB_RENDER_WORK="$WORK/render" \
  "$HERE/tools/render/render.sh" MRQ_Still_Embarcadero || true
cp "$OB_FRAMES_DIR"/MRQ_Still_Embarcadero/sf_*.png "$WORK"/ 2>/dev/null || true
ls "$WORK"/sf_*.png 2>/dev/null || echo "(no stills written)"

echo "5/5 terrain declaration + verification header"
"$PY" "$HERE/tools/levels/embarcadero/write_terrain.py" "$HERE"
echo "done"
