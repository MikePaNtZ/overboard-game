#!/bin/bash
# Build OB_Trail from a MuJoCo course directory, end to end. One command:
#
#   tools/trail/build_trail.sh <course_dir> [track.npz]
#
#   course_dir  an overboard-viz carve-lab course (course_height.npy, metadata.json, course.json,
#               lane.json), for example .../carve-lab/data/courses/valley_gentle
#   track.npz   a MuJoCo run on that course (carve-lab track keys). Without one, a PLACEHOLDER
#               track is generated: a straight run down the path at 3 m/s, for scene and camera
#               checks only.
#
# Steps: gen_course.py (landscape, path, creek, bridge, dressing) -> gen_foliage.py (grass and
# flowers) -> the track -> plan_cameras.py --shots trail -> build_trail_level.py in the editor ->
# verify_trail.py (the track rides on the landscape). Work files: /tmp/ob-trail/. Then render:
#   tools/trail/render_trail.sh            (the stills, 1080p)
set -eu
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PY:-/Users/mike/projects/overboard/.venv/bin/python}"
COURSE="$(cd "$1" && pwd)"
NAME="$(basename "$COURSE")"
WORK=/tmp/ob-trail
DATA="$WORK/$NAME"
mkdir -p "$WORK"

echo "1/6 course -> $DATA"
"$PY" "$HERE/tools/trail/gen_course.py" "$COURSE" "$DATA" | tail -4
echo "2/6 foliage"
"$PY" "$HERE/tools/trail/gen_foliage.py" "$WORK/foliage" > /dev/null
echo "3/6 track"
if [ $# -ge 2 ]; then
  cp "$2" "$WORK/track.npz"
else
  "$PY" "$HERE/tools/trail/gen_track.py" "$COURSE" "$WORK/track.npz"
fi
"$PY" "$HERE/tools/replay/npz_replay.py" "$WORK/track.npz" --bin "$WORK/track.bin"
echo "4/6 cameras"
"$PY" "$HERE/tools/render/plan_cameras.py" "$WORK/track.npz" "$WORK/cameras.json" --shots trail --origin-cm 0,0,0 --origin-yaw 0

echo "5/6 level (editor, offscreen; several minutes)"
# The builder owns these paths and rebuilds them whole. The editor cannot delete a map it may
# hold, so they go before it starts.
rm -rf "$HERE/Content/Maps/OB_Trail.umap" "$HERE/Content/Maps/OB_Trail_BuiltData.uasset" \
  "$HERE/Content/Trail/Cinematics" "$HERE/Content/Trail/Meshes" "$HERE/Content/Trail/Landscape"
export OB_TRAIL_DATA="$DATA" OB_TRAIL_FOLIAGE="$WORK/foliage" OB_CAMERAS="$WORK/cameras.json"
"$HERE/tools/trail/ue.sh" "$HERE/tools/trail/build_trail_level.py" || { tail -20 "$WORK/build.txt"; exit 1; }
grep -E "COMPILE ERRORS|MISSING|FAIL" "$WORK/build.txt" && echo "(see $WORK/build.txt)"

echo "6/6 verify"
OB_TRACK_BIN="$WORK/track.bin" "$HERE/tools/trail/ue.sh" "$HERE/tools/trail/verify_trail.py" || true
cat "$WORK/verify.txt"
