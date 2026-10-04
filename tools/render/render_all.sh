#!/bin/bash
# Render every shot of SEQ_Carve at final quality, one MRQ job per camera cut (see the note in
# build_carve_level.py), into $OB_FRAMES_DIR/MRQ_Final_shots/. Shot names come from the configs.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
FRAMES="${OB_FRAMES_DIR:-/tmp/ob-render/frames}"
rm -rf "${FRAMES:?}/MRQ_Final_shots"
for SHOT in "$@"; do
  echo "shot $SHOT"
  "$HERE/render.sh" "MRQ_Final_$SHOT"
done
ls "$FRAMES/MRQ_Final_shots" | wc -l
