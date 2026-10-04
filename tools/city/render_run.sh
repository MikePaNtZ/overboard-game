#!/bin/bash
# Render one recorded run in OB_CityHill with the follow camera and the rider HUD, as an mp4.
# usage: tools/city/render_run.sh <work_dir> [Preview|Final]
#   <work_dir> holds track.npz (with the controls track's HUD columns), track.bin (npz_replay.py --bin)
#   and cameras.json (plan_cameras.py --shots follow). It gets frames/, hud/, render logs and run.mp4.
#   The street data comes from $OB_CITY_DATA (default /tmp/ob-city/city_hill, made by build_city.sh).
# The level's sequence follows the run's camera plan, so runs render one after another, not in parallel.
set -eu
W="$(cd "$1" && pwd)"; Q="${2:-Preview}"
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
PY="${PY:-/Users/mike/projects/overboard/.venv/bin/python}"
export OB_CITY_WORK="$W" OB_CAMERAS="$W/cameras.json" OB_FRAMES_DIR="$W/frames" OB_BUILD_LOG="$W/build.txt"
export OB_CITY_DATA="${OB_CITY_DATA:-/tmp/ob-city/city_hill}" OB_CITY_SKIP="${OB_CITY_SKIP:-massing}"
rm -rf "$HERE/Content/Maps/OB_CityHill.umap" "$HERE/Content/Maps/OB_CityHill_BuiltData.uasset" \
  "$HERE/Content/CityHill/Cinematics" "$HERE/Content/CityHill/Meshes" "$HERE/Content/CityHill/Materials"
"$HERE/tools/city/ue.sh" "$HERE/tools/city/build_city_level.py" >/dev/null
export OB_EXTRA="${OB_EXTRA:--ObRenderRider -ObRiderLights -ObKeyCd=20000 -ObRimCd=40000 -ObBoardSkin=x7}"
"$HERE/tools/city/render_city.sh" "$Q" F1_follow
"$PY" "$HERE/tools/hud/render_hud.py" "$W/track.npz" "$W/cameras.json" F1_follow "$W/hud" --over "$W/frames/MRQ_${Q}_F1_follow"
ffmpeg -v error -y -framerate 24 -pattern_type glob -i "$W/hud/*.jpg" -c:v libx264 -crf 18 -pix_fmt yuv420p "$W/run.mp4"
echo "$W/run.mp4"
