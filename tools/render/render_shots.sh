#!/bin/bash
# Render shots of SEQ_Carve, one MRQ job per camera cut, each with its own replay clock.
# usage: tools/render/render_shots.sh <Preview|Final> [shot names...]   (default: every shot)
# Frames land in $OB_FRAMES_DIR/MRQ_<Quality>_shots/. Per-shot replay offset and rate come from
# the camera plan ($OB_CAMERAS, written by plan_cameras.py), so slow-motion shots stay in sync.
# Extra UE args: $OB_EXTRA (for example "-ObRenderRider -ObRiderLights").
set -u
Q=$1; shift
HERE="$(cd "$(dirname "$0")" && pwd)"
CAMS="${OB_CAMERAS:-/tmp/ob-render/cameras.json}"
FRAMES="${OB_FRAMES_DIR:-/tmp/ob-render/frames}"
SHOTS="$*"
if [ -z "$SHOTS" ]; then
  SHOTS=$(python3 -c "import json,sys;print(' '.join(s['name'] for s in json.load(open(sys.argv[1]))['shots']))" "$CAMS")
fi
if [ "${OB_KEEP:-0}" != 1 ]; then rm -rf "${FRAMES:?}/MRQ_${Q}_shots"; fi
for SHOT in $SHOTS; do
  read OFFSET RATE < <(python3 -c "import json,sys;s=[s for s in json.load(open(sys.argv[1]))['shots'] if s['name']==sys.argv[2]][0];print(s['replay_offset'],s['replay_rate'])" "$CAMS" "$SHOT")
  echo "shot $SHOT offset $OFFSET rate $RATE"
  OB_REPLAY_OFFSET=$OFFSET "$HERE/render.sh" "MRQ_${Q}_$SHOT" -ObReplayRate=$RATE ${OB_EXTRA:-}
done
ls "$FRAMES/MRQ_${Q}_shots" | wc -l
