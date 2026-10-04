#!/usr/bin/env bash
# One command to ride: starts sim-host in the background, opens the game in the Unreal editor
# (default level OB_CityHill), and stops sim-host when the editor closes.
#
#   tools/play/play.sh          # editor; press Play, then Cross (or Space) to arm
#   tools/play/play.sh --game   # straight into the game window, no editor
#
# The sim log goes to /tmp/overboard-play-sim.log. Extra sim flags: SIM_ARGS="--trace-csv /tmp/t.csv".
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
UE="/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"
LOG=/tmp/overboard-play-sim.log

if pgrep -f "target/release/sim-host" > /dev/null; then
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

if [[ "${1:-}" == "--game" ]]; then
  "$UE" "$HERE/OverboardGame.uproject" /Game/Maps/OB_CityHill -game -windowed -ResX=1920 -ResY=1080
else
  "$UE" "$HERE/OverboardGame.uproject"
fi
echo "Game closed; stopping sim-host."
