#!/usr/bin/env bash
# One command to ride: starts sim-host in the background, opens the game in the Unreal editor
# (default level OB_CityHill), and stops sim-host when the editor closes.
#
#   tools/play/play.sh          # editor; press Play, then Cross (or Space) to arm
#   tools/play/play.sh --game   # straight into the game window, no editor
#   tools/play/play.sh --level parking_lot [--game]   # another level (tools/play/levels/*.env)
#
# The sim log goes to /tmp/overboard-play-sim.log. Extra sim flags: SIM_ARGS="--trace-csv /tmp/t.csv".
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
UE="/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
LOG=/tmp/overboard-play-sim.log

GAME=0
export LEVEL=city_hill
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game) GAME=1 ;;
    --level) LEVEL="$2"; shift ;;
    *) echo "unknown argument: $1"; exit 1 ;;
  esac
  shift
done
PLAY_DIR="$HERE/tools/play"
[[ -f "$PLAY_DIR/levels/$LEVEL.env" ]] || { echo "no level file $PLAY_DIR/levels/$LEVEL.env"; exit 1; }
# shellcheck disable=SC1090
source "$PLAY_DIR/levels/$LEVEL.env"

if pgrep -f "sim-host.*127.0.0.1:9601" > /dev/null; then
  echo "A sim-host is already running (it holds port 9601/9602). Stop it first:"
  pgrep -fl "target/release/sim-host"
  exit 1
fi

# shellcheck disable=SC2086
"$HERE/tools/play/run_sim.sh" ${SIM_ARGS:-} > "$LOG" 2>&1 &
SIM=$!
trap 'kill -TERM $SIM 2>/dev/null || true' EXIT
sleep 1
if ! kill -0 $SIM 2>/dev/null; then
  echo "sim-host did not start. Last lines of $LOG:"
  tail -5 "$LOG"
  exit 1
fi
echo "sim-host running (pid $SIM, log $LOG). Opening the game..."

# The MetaHuman skater on the X7 board, as in the renders (the game's default is the mannequin),
# and a per-frame ride log of the last ride for diagnosis (/tmp/overboard-ride.csv).
LOOK="-ObRenderRider -ObBoardSkin=x7 -ObRideLog=/tmp/overboard-ride.csv $COURSE_ARG"
if [[ $GAME == 1 ]]; then
  # shellcheck disable=SC2086
  "$UE" "$HERE/OverboardGame.uproject" "$MAP" -game -windowed -ResX=1920 -ResY=1080 $LOOK
elif [[ $LEVEL == city_hill ]]; then
  # shellcheck disable=SC2086
  "$UE" "$HERE/OverboardGame.uproject" $LOOK
else
  # shellcheck disable=SC2086
  "$UE" "$HERE/OverboardGame.uproject" "$MAP" $LOOK
fi
echo "Game closed; stopping sim-host."
