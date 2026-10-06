#!/usr/bin/env bash
# Start sim-host for live play. LEVEL picks tools/play/levels/<LEVEL>.env (default city_hill).
# sim-host owns all board physics. Unreal only sends inputs and draws the state.
# Flags confirmed with the controls track (c4), 2026-10-05. sim-host is built from
# overboard master, PINNED at cd7962d (controls PR #298: lean lag, carving model with camber thrust): stiff balance gains (Kp 420) with the tail drag
# back under a full lean (--rider-reach-back 0.20), pad-mode pull-away, and fixed obstacles from
# elements/city_hill_obstacles.csv (regenerate with <overboard-carve>/sim/carve/obstacles.py after
# changing elements/city_hill.json). See docs/playable-status.md.
# Extra arguments go to sim-host (for example --trace-csv PATH or --host-stats PATH).
set -euo pipefail
STATE_OUT=${STATE_OUT:-127.0.0.1:9601}
INPUT_IN=${INPUT_IN:-127.0.0.1:9602}
# The second, optional packet (OBHD): battery, pack voltage and the authority margin for the HUD.
HUD_OUT=${HUD_OUT:-127.0.0.1:9603}
PLAY_DIR="$(cd "$(dirname "$0")" && pwd)"
LEVEL=${LEVEL:-city_hill}
[[ -f "$PLAY_DIR/levels/$LEVEL.env" ]] || { echo "no level file $PLAY_DIR/levels/$LEVEL.env" >&2; exit 1; }
# shellcheck disable=SC1090
source "$PLAY_DIR/levels/$LEVEL.env"
source "$PLAY_DIR/env.sh"
exec "$BIN/sim-host" \
  --lean-steer --estimator-aiding grade-aware \
  --terrain "$COURSE/course_hfield.bin" ${SPAWN_ARGS} \
  --tail-brake --tail-friction 0.6 --hold-until-arm --balance-comp \
  --plant x7 --authority-margin warn --max-current 90 --rider-mass 95 --rider-reach 0.10 --rider-reach-back 0.20 --rider-lean-lag 0.25 \
  --obstacles "$OBSTACLES" \
  --state-out-addr "$STATE_OUT" --input-in-addr "$INPUT_IN" --hud-out-addr "$HUD_OUT" \
  "$@"
