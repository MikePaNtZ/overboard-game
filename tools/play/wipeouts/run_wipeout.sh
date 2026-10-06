#!/usr/bin/env bash
# Replay one MuJoCo wipeout case into the game (headless) and compare the Unreal ragdoll's rest
# points with MuJoCo's. sim-host must NOT be running: replay_wipeout.py sends the state instead.
#   tools/play/wipeouts/run_wipeout.sh <case> [extra game args...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../../.." && pwd)"
CASE=$1; shift
UE="/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
LOG="/tmp/wipeout-$CASE.log"
export PORT_BASE=${PORT_BASE:-9600}
if pgrep -f "sim-host.*127.0.0.1:$((PORT_BASE + 1))" > /dev/null; then echo "a sim-host runs on port base $PORT_BASE"; exit 1; fi

"$UE" "$HERE/OverboardGame.uproject" /Game/Maps/OB_CityHill -game -nullrhi -unattended -nosplash \
  -ObRenderRider -ObBoardSkin=x7 -ObPortBase=$PORT_BASE "$@" -abslog="$LOG" > /dev/null 2>&1 &
GAME=$!
trap 'kill $GAME 2>/dev/null || true' EXIT
# The game needs ~45 s to load; park the board at the first frame until then.
python3 "$HERE/tools/play/wipeouts/replay_wipeout.py" "$CASE" --wait 60 --lead 2.0 --hold 9.0
sleep 1
python3 "$HERE/tools/play/wipeouts/compare_wipeout.py" "$CASE" "$LOG"
