#!/bin/bash
# Render OB_CityHill stills (or look-dev frames) with Movie Render Queue, one job per shot.
# usage: tools/city/render_city.sh [Still|Look|Final] [shot names...]   (default: Still, every shot)
#   Still  1920x1080, 2 spatial x 16 temporal samples, the shot's still frame
#   Look   960x540, 4 temporal samples, the same frame (fast look-dev)
#   Final  1920x1080, 8 temporal samples, the whole shot
#   Preview 960x540, 4 temporal samples, the whole shot (a fast sanity check)
# Frames land in /tmp/ob-city/frames/MRQ_<Q>_<shot>/. The board replays /tmp/ob-city/track.bin on
# the camera plan's clock (/tmp/ob-city/cameras.json), both written by build_city.sh.
# Extra UE args: $OB_EXTRA (default: the MetaHuman skater rider). For OB_CityHill use
#   OB_EXTRA="-ObRenderRider -ObRiderLights -ObKeyCd=20000 -ObRimCd=40000"
set -u
Q=${1:-Still}; shift || true
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${OB_CITY_WORK:-/tmp/ob-city}"
CAMS="$WORK/cameras.json"
SHOTS="$*"
[ -z "$SHOTS" ] && SHOTS=$(python3 -c "import json;print(' '.join(s['name'] for s in json.load(open('$CAMS'))['shots']))")
export OB_MAP=/Game/Maps/OB_CityHill OB_SEQ=/Game/CityHill/Cinematics/SEQ_CityHill OB_CINE=/Game/CityHill/Cinematics
export OB_REPLAY="$WORK/track.bin" OB_FRAMES_DIR="$WORK/frames" OB_RENDER_WORK="$WORK/render"
for SHOT in $SHOTS; do
  read OFFSET RATE < <(python3 -c "import json,sys;s=[s for s in json.load(open('$CAMS'))['shots'] if s['name']==sys.argv[1]][0];print(s['replay_offset'],s['replay_rate'])" "$SHOT")
  echo "$Q $SHOT (replay offset $OFFSET, rate $RATE)"
  OB_REPLAY_OFFSET=$OFFSET "$HERE/tools/render/render.sh" "MRQ_${Q}_$SHOT" -ObReplayRate=$RATE ${OB_EXTRA:--ObRenderRider}
done
