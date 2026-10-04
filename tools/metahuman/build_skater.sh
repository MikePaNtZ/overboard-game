#!/bin/bash
# Build the MetaHuman skater rider end to end. One command:
#   tools/metahuman/build_skater.sh          (all steps; "build_skater.sh 3" runs step 3 only)
# 1. create_skater.py      headless commandlet: the MetaHuman Character (face, body, wardrobe, grooms)
# 2. finish_skater.py      full editor, offscreen: Epic cloud auto-rig + texture sources, then the
#                          Cinematic build to /Game/MetaHumans/Skater/BP_Skater
# 3. post_build_skater.py  full editor, offscreen: retarget the riding animation, bind the hair groom
# The cloud step needs an Epic sign-in. The first time, the log asks for a device code; a stored
# login is reused after that. Logs: /tmp/ob-render/mh_*.txt
set -u
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
ENGINE="/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac"
UPROJECT="$HERE/OverboardGame.uproject"
mkdir -p /tmp/ob-render
run_cmdlet() {
  "$ENGINE/UnrealEditor-Cmd" "$UPROJECT" -run=pythonscript -script="$HERE/tools/metahuman/$1" \
    -stdout -unattended -nosplash > "/tmp/ob-render/mh_${1%.py}.log" 2>&1
}
STEPS="${1:-123}"
if [[ $STEPS == *1* ]]; then
echo "1/3 create";   run_cmdlet create_skater.py;   cat /tmp/ob-render/mh_create.txt
fi
# Steps 2 and 3 run in the editor, offscreen: in a commandlet the material bake crashes (no
# Texture Graph engine) and the batch retarget asserts (no Slate); with no display attached a
# normal editor window blocks in Metal Present.
run_editor() {  # <script> <result file>; the result file's last line starts OK, FAIL or EXCEPTION
  rm -f "$2"
  "$ENGINE/UnrealEditor.app/Contents/MacOS/UnrealEditor" "$UPROJECT" \
    -ExecutePythonScript="$HERE/tools/metahuman/$1" -OBQuitWhenDone -RenderOffscreen \
    -unattended -nosplash -log -abslog="/tmp/ob-render/mh_${1%.py}.log" >/dev/null 2>&1 &
  local pid=$!
  until ! kill -0 $pid 2>/dev/null || grep -qE "^(OK|FAIL|EXCEPTION)" "$2" 2>/dev/null; do sleep 5; done
  # The editor can crash or hang while it shuts down (seen 2026-10-04, after the work was saved).
  for _ in $(seq 1 24); do kill -0 $pid 2>/dev/null || break; sleep 5; done
  kill -9 $pid 2>/dev/null
  wait $pid 2>/dev/null
}
if [[ $STEPS == *2* ]]; then
echo "2/3 finish";   run_editor finish_skater.py /tmp/ob-render/mh_finish.txt
cat /tmp/ob-render/mh_finish.txt
grep -q "^OK" /tmp/ob-render/mh_finish.txt || { echo "finish step failed; see /tmp/ob-render/mh_finish_skater.log"; exit 1; }
fi
if [[ $STEPS == *3* ]]; then
echo "3/3 post-build"; run_editor post_build_skater.py /tmp/ob-render/mh_post_build.txt; cat /tmp/ob-render/mh_post_build.txt
fi
