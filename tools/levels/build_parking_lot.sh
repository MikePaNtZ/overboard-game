#!/bin/bash
# Build OB_ParkingLot (Level 1) from the parking_lot course, end to end. One command, re-runnable:
#
#   tools/levels/build_parking_lot.sh [course_dir]
#
# Steps: gen_parking_lot.py (numpy: the ground tiles, boxes, cones, markings) -> build_parking_lot.py
# in the full editor (meshes, props, dressing, daylight, the map) -> verify_parking_lot.py (the drawn
# ground vs the course height along the demo path) -> stills_parking_lot.py (review PNGs) -> the
# terrain declaration + the regenerated verification header. Work files: /tmp/ob-levels-map/.
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PY:-/Users/mike/projects/overboard/.venv/bin/python}"
COURSE="${1:-/Users/mike/projects/overboard-viz/out/carve-lab/data/courses/parking_lot}"
COURSE="$(cd "$COURSE" && pwd)"
WORK="${OB_LEVELS_WORK:-/tmp/ob-levels-map}"
DATA="$WORK/data"
export OB_LEVELS_WORK="$WORK" OB_PL_DATA="$DATA" OB_COURSE_DIR="$COURSE" OB_BUILD_LOG="$WORK/build.txt"
export OB_FRAMES_DIR="$WORK/frames"
mkdir -p "$WORK" "$DATA"

echo "1/5 course -> $DATA (numpy)"
"$PY" "$HERE/tools/levels/gen_parking_lot.py" "$COURSE" "$DATA" | tail -10

echo "2/5 level (editor, offscreen; several minutes)"
rm -rf "$HERE/Content/Maps/OB_ParkingLot.umap" "$HERE/Content/Maps/OB_ParkingLot_BuiltData.uasset" \
  "$HERE/Content/ParkingLot/Meshes" "$HERE/Content/ParkingLot/Materials"
"$HERE/tools/levels/ue.sh" "$HERE/tools/levels/build_parking_lot.py" || { tail -25 "$WORK/build.txt"; exit 1; }
grep -E "COMPILE ERRORS|MISSING|FAIL" "$WORK/build.txt" && echo "(see $WORK/build.txt)" || true

echo "3/5 verify (numpy: the drawn ground vs the course height)"
"$PY" "$HERE/tools/levels/verify_parking_lot.py" | tail -4

echo "4/5 stills (editor builds the camera sequence, then MRQ renders offscreen)"
"$HERE/tools/levels/ue.sh" "$HERE/tools/levels/stills_parking_lot.py" || { tail -15 "$WORK/editor_stills_parking_lot.log"; true; }
OB_MAP=/Game/Maps/OB_ParkingLot OB_SEQ=/Game/ParkingLot/Cinematics/SEQ_ParkingLot \
  OB_CINE=/Game/ParkingLot/Cinematics OB_REPLAY=/dev/null OB_RENDER_WORK="$WORK/render" \
  "$HERE/tools/render/render.sh" MRQ_Still_ParkingLot || true
cp "$OB_FRAMES_DIR"/MRQ_Still_ParkingLot/pl_*.png "$WORK"/ 2>/dev/null || true
ls "$WORK"/pl_*.png 2>/dev/null || echo "(no stills written)"

echo "5/5 terrain declaration + verification header"
"$PY" "$HERE/tools/levels/write_terrain_decl.py" "$HERE"
echo "done"
