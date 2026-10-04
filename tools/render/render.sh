#!/bin/bash
# Render the carve sequence with Movie Render Queue, headless-ish (-game opens a window).
# usage: tools/render/render.sh <ConfigName> [extra UE args]
#   ConfigName: MRQ_Preview | MRQ_Final | MRQ_Still  (assets in /Game/Cinematics, built by
#   build_carve_level.py). Frames land in $OB_FRAMES_DIR/<ConfigName>/ (default /tmp/ob-render/frames).
#
# The board is driven by the replay file, not by UDP: -ObReplay=<bin> -ObReplayOffset=<sim s>.
# Make the bin with tools/replay/npz_replay.py --bin.
set -u
CFG=$1; shift
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
REPLAY="${OB_REPLAY:-/tmp/ob-render/a05.bin}"
OFFSET="${OB_REPLAY_OFFSET:-3.0}"
FRAMES="${OB_FRAMES_DIR:-/tmp/ob-render/frames}"
LOG="/tmp/ob-render/render_${CFG}.log"
rm -rf "${FRAMES:?}/${CFG}"
"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$HERE/OverboardGame.uproject" /Game/Maps/OB_Carve -game \
  -LevelSequence=/Game/Cinematics/SEQ_Carve.SEQ_Carve \
  -MoviePipelineConfig=/Game/Cinematics/${CFG}.${CFG} \
  -windowed -resx=960 -resy=540 -log -unattended -nosplash -nosound -NoLoadingScreen \
  -abslog="$LOG" -ObReplay="$REPLAY" -ObReplayOffset="$OFFSET" "$@" >/dev/null 2>&1
echo "exit $?"
ls "$FRAMES/$CFG" 2>/dev/null | wc -l
