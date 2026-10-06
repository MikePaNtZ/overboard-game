#!/usr/bin/env bash
# Record a demo ride of the live game: sim-host (MuJoCo) + the game on OB_CityHill, with the
# demo rider playing the pad (-ObDemoRider) and the HUD and game elements on screen.
# Everything is live and real time; the game records its own viewport (FGameVideoRecorder).
#
#   tools/play/make_demo_video.sh [--level NAME] [out.mp4] [WxH] [fps]
#   default: /tmp/overboard-demo.mp4 1920x1080 30
# Logs: /tmp/overboard-demo-game.log, /tmp/overboard-demo-sim.log, trace /tmp/overboard-demo-trace.csv
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
export LEVEL=city_hill
if [[ "${1:-}" == "--level" ]]; then LEVEL="$2"; shift 2; fi
PLAY_DIR="$HERE/tools/play"
[[ -f "$PLAY_DIR/levels/$LEVEL.env" ]] || { echo "no level file $PLAY_DIR/levels/$LEVEL.env"; exit 1; }
# shellcheck disable=SC1090
source "$PLAY_DIR/levels/$LEVEL.env"
OUT=${1:-/tmp/overboard-demo.mp4}
RES=${2:-1920x1080}
FPS=${3:-30}
UE="/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"

if pgrep -f "sim-host.*127.0.0.1:9601" > /dev/null; then
  echo "A sim-host is already running (ports 9601/9602). Stop it first."; exit 1
fi

"$HERE/tools/play/run_sim.sh" --trace-csv /tmp/overboard-demo-trace.csv > /tmp/overboard-demo-sim.log 2>&1 &
SIM=$!
trap 'kill -TERM $SIM 2>/dev/null || true' EXIT
sleep 2

# The demo rider closes the video and quits the game when its script ends.
# shellcheck disable=SC2086
"$UE" "$HERE/OverboardGame.uproject" "$MAP" -game -RenderOffscreen $COURSE_ARG \
  -ResX="${RES%x*}" -ResY="${RES#*x}" -ForceRes -nosound -unattended \
  -ObDemoRider -ObRenderRider -ObBoardSkin=x7 -ExecCmds="DisableAllScreenMessages" -ObRecordVideo="$OUT" -ObRecordFps="$FPS" -abslog=/tmp/overboard-demo-game.log > /dev/null 2>&1 || true
echo "wrote $OUT"
