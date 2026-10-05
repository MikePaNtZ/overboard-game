#!/bin/bash
# Run a tools/trail editor script in the full editor, offscreen, and wait for it.
# usage: tools/trail/ue.sh <script.py>      (environment passes through; see build_trail.sh)
# The editor can hang while it shuts down after the work is saved; the wait loop kills it then.
set -u
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
RESULT=/tmp/ob-trail/editor_result.txt
LOG=/tmp/ob-trail/editor_$(basename "$1" .py).log
mkdir -p /tmp/ob-trail
rm -f "$RESULT"
OB_EDITOR_SCRIPT="$SCRIPT" OB_EDITOR_RESULT="$RESULT" \
  "/Users/Shared/Epic Games/UE_5.7/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
  "$HERE/OverboardGame.uproject" /Game/Maps/OB_Main -ExecutePythonScript="$HERE/tools/trail/run_in_editor.py" -RenderOffscreen \
  -unattended -nosplash -nosound -log -abslog="$LOG" >/dev/null 2>&1 &
PID=$!
until ! kill -0 $PID 2>/dev/null || [ -s "$RESULT" ]; do sleep 3; done
for _ in $(seq 1 40); do kill -0 $PID 2>/dev/null || break; sleep 3; done
kill -9 $PID 2>/dev/null
wait $PID 2>/dev/null
tail -1 "$RESULT" 2>/dev/null || echo "NO RESULT (editor died); see $LOG"
[ "$(tail -1 "$RESULT" 2>/dev/null)" = "OK" ]
