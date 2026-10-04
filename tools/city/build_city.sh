#!/bin/bash
# Build OB_CityHill from a MuJoCo course directory, end to end. One command:
#
#   tools/city/build_city.sh <course_dir> [track.npz]
#
#   course_dir  an overboard-viz carve-lab course (course_height.npy, metadata.json, course.json,
#               lane.json), for example .../carve-lab/data/courses/city_hill
#   track.npz   a MuJoCo run on that course. Without one, a PLACEHOLDER track is generated (a
#               straight run down the street centre at 3 m/s), for scene and camera checks only.
#
# Steps: measure_buildings.py (the building footprints, editor) -> gen_city.py (street, kerbs,
# sidewalks, massing, dressing) -> the track -> plan_cameras.py --shots follow -> build_city_level.py
# in the editor -> verify_city.py (the tyre rides on the street). Work files: /tmp/ob-city/. Then:
#   tools/city/render_city.sh Look            (a fast look-dev frame per shot)
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PY:-/Users/mike/projects/overboard/.venv/bin/python}"
COURSE="$(cd "$1" && pwd)"
NAME="$(basename "$COURSE")"
WORK="${OB_CITY_WORK:-/tmp/ob-city}"   # set OB_CITY_WORK per worktree to keep runs apart
export OB_CITY_WORK="$WORK" OB_BUILD_LOG="$WORK/build.txt" OB_FRAMES_DIR="$WORK/frames"
DATA="$WORK/$NAME"
mkdir -p "$WORK"

echo "1/6 building footprints (editor; several minutes)"
OB_CITY_BOUNDS="$WORK/building_bounds.json" "$HERE/tools/city/ue.sh" "$HERE/tools/city/measure_buildings.py" || true
echo "2/6 course -> $DATA"
"$PY" "$HERE/tools/city/gen_city.py" "$COURSE" "$DATA" | tail -8
echo "3/6 track"
if [ $# -ge 2 ]; then
  cp "$2" "$WORK/track.npz"
else
  "$PY" "$HERE/tools/trail/gen_track.py" "$COURSE" "$WORK/track.npz"
fi
"$PY" "$HERE/tools/replay/npz_replay.py" "$WORK/track.npz" --bin "$WORK/track.bin"
echo "4/6 cameras (one follow shot)"
OB_SCATTER="$DATA/scatter.json" "$PY" "$HERE/tools/render/plan_cameras.py" "$WORK/track.npz" "$WORK/cameras.json" \
  --shots follow --origin-cm 0,0,0 --origin-yaw 0

echo "5/6 level (editor, offscreen; several minutes)"
rm -rf "$HERE/Content/Maps/OB_CityHill.umap" "$HERE/Content/Maps/OB_CityHill_BuiltData.uasset" \
  "$HERE/Content/CityHill/Cinematics" "$HERE/Content/CityHill/Meshes" "$HERE/Content/CityHill/Materials"
export OB_CITY_DATA="$DATA" OB_CAMERAS="$WORK/cameras.json"
"$HERE/tools/city/ue.sh" "$HERE/tools/city/build_city_level.py" || { tail -20 "$WORK/build.txt"; exit 1; }
grep -E "COMPILE ERRORS|MISSING|FAIL" "$WORK/build.txt" && echo "(see $WORK/build.txt)" || true

echo "6/6 verify (numpy; the street mesh is the course height, so the check is analytical)"
OB_CITY_DATA="$DATA" "$PY" "$HERE/tools/city/verify_city.py" "$WORK/track.bin" | tail -4
