#!/usr/bin/env bash
# Start sim-host for live play on the city_hill course.
# sim-host owns all board physics. Unreal only sends inputs and draws the state.
# Flags confirmed with the controls track (c4), 2026-10-05. sim-host is built from
# feat/controls/downhill-carve, PINNED at ddb1535: stiff balance gains (Kp 420) with the tail drag
# back under a full lean (--rider-reach-back 0.20), pad-mode pull-away, and fixed obstacles from
# elements/city_hill_obstacles.csv (regenerate with <overboard-carve>/sim/carve/obstacles.py after
# changing elements/city_hill.json). See docs/playable-status.md.
# Extra arguments go to sim-host (for example --trace-csv PATH or --host-stats PATH).
set -euo pipefail
STATE_OUT=${STATE_OUT:-127.0.0.1:9601}
INPUT_IN=${INPUT_IN:-127.0.0.1:9602}
# The second, optional packet (OBHD): battery, pack voltage and the authority margin for the HUD.
HUD_OUT=${HUD_OUT:-127.0.0.1:9603}
source "$(dirname "$0")/env.sh"
exec "$BIN/sim-host" \
  --lean-steer --estimator-aiding grade-aware \
  --terrain "$COURSE/course_hfield.bin" --spawn-x 88 \
  --tail-brake --tail-friction 0.6 --hold-until-arm --balance-comp \
  --plant x7 --authority-margin warn --max-current 90 --rider-mass 95 --rider-reach 0.10 --rider-reach-back 0.20 \
  --obstacles "$(dirname "$0")/elements/city_hill_obstacles.csv" \
  --state-out-addr "$STATE_OUT" --input-in-addr "$INPUT_IN" --hud-out-addr "$HUD_OUT" \
  "$@"
