#!/bin/bash
# Render OB_Trail stills (or look-dev frames) with Movie Render Queue, one job per shot.
# usage: tools/trail/render_trail.sh [Still|Look|Final] [shot names...]   (default: Still, every shot)
#   Still  1920x1080, 2 spatial x 16 temporal samples, the shot's still frame
#   Look   960x540, 4 temporal samples, the same frame (fast look-dev)
#   Final  1920x1080, 8 temporal samples, the whole shot
# Frames land in /tmp/ob-trail/frames/MRQ_<Q>_<shot>/. The board replays /tmp/ob-trail/track.bin on
# the camera plan's clock (/tmp/ob-trail/cameras.json), both written by build_trail.sh.
# Extra UE args: $OB_EXTRA (default: the MetaHuman skater rider).
set -u
Q=${1:-Still}; shift || true
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
CAMS=/tmp/ob-trail/cameras.json
SHOTS="$*"
[ -z "$SHOTS" ] && SHOTS=$(python3 -c "import json;print(' '.join(s['name'] for s in json.load(open('$CAMS'))['shots']))")
export OB_MAP=/Game/Maps/OB_Trail OB_SEQ=/Game/Trail/Cinematics/SEQ_Trail OB_CINE=/Game/Trail/Cinematics
export OB_REPLAY=/tmp/ob-trail/track.bin OB_FRAMES_DIR=/tmp/ob-trail/frames
for SHOT in $SHOTS; do
  read OFFSET RATE < <(python3 -c "import json,sys;s=[s for s in json.load(open('$CAMS'))['shots'] if s['name']==sys.argv[1]][0];print(s['replay_offset'],s['replay_rate'])" "$SHOT")
  echo "$Q $SHOT (replay offset $OFFSET, rate $RATE)"
  OB_REPLAY_OFFSET=$OFFSET "$HERE/tools/render/render.sh" "MRQ_${Q}_$SHOT" -ObReplayRate=$RATE ${OB_EXTRA:--ObRenderRider}
done
