#!/bin/bash
# Render the carve sequence with Movie Render Queue, headless-ish (-game opens a window).
# usage: tools/render/render.sh <ConfigName> [extra UE args]
#   OB_MAP, OB_SEQ and OB_CINE select another level (default OB_Carve). OB_Trail:
#   OB_MAP=/Game/Maps/OB_Trail OB_SEQ=/Game/Trail/Cinematics/SEQ_Trail OB_CINE=/Game/Trail/Cinematics
#   ConfigName: MRQ_Preview | MRQ_Final | MRQ_Still  (assets in /Game/Cinematics, built by
#   build_carve_level.py). Frames land in $OB_FRAMES_DIR/<ConfigName>/ (default /tmp/ob-render/frames).
#
# The board is driven by the replay file, not by UDP: -ObReplay=<bin> -ObReplayOffset=<sim s>.
# Make the bin with tools/replay/npz_replay.py --bin.
# -RenderOffscreen: MRQ needs no window, and with no display attached a window blocks in Metal Present.
set -u
CFG=$1; shift
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
REPLAY="${OB_REPLAY:-/tmp/ob-render/a05.bin}"
OFFSET="${OB_REPLAY_OFFSET:-3.0}"
FRAMES="${OB_FRAMES_DIR:-/tmp/ob-render/frames}"
LOG="/tmp/ob-render/render_${CFG}.log"
MAP="${OB_MAP:-/Game/Maps/OB_Carve}"
SEQ="${OB_SEQ:-/Game/Cinematics/SEQ_Carve}"
CINE="${OB_CINE:-/Game/Cinematics}"
mkdir -p /tmp/ob-render
rm -rf "${FRAMES:?}/${CFG}"
"/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$HERE/OverboardGame.uproject" "$MAP" -game \
  -LevelSequence="$SEQ.${SEQ##*/}" \
  -MoviePipelineConfig="$CINE/${CFG}.${CFG}" \
  -windowed -resx=960 -resy=540 -RenderOffscreen -log -unattended -nosplash -nosound -NoLoadingScreen \
  -abslog="$LOG" -ObReplay="$REPLAY" -ObReplayOffset="$OFFSET" "$@" >/dev/null 2>&1
echo "exit $?"
ls "$FRAMES/$CFG" 2>/dev/null | wc -l
