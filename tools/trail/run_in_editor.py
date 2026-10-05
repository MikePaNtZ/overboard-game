# run_in_editor.py -- runs a tools/trail script inside the full editor (shaders compile there, which
# a commandlet cannot do), then quits. The script to run is in the environment:
#   OB_EDITOR_SCRIPT=<abs path> UnrealEditor <uproject> -ExecutePythonScript=<this file> -RenderOffscreen
# The last line of $OB_EDITOR_RESULT (default /tmp/ob-trail/editor_result.txt) is OK or EXCEPTION.
import os
import runpy
import traceback

import unreal

result = os.environ.get("OB_EDITOR_RESULT", "/tmp/ob-trail/editor_result.txt")
script = os.environ["OB_EDITOR_SCRIPT"]
try:
    runpy.run_path(script, run_name="__main__")
    status = "OK"
except SystemExit as e:
    status = "OK" if not e.code else "EXCEPTION\nSystemExit(%s)" % e.code
except Exception:
    status = "EXCEPTION\n" + traceback.format_exc()
with open(result, "w") as f:
    f.write(status.split("\n", 1)[-1] + "\n" + status.split("\n", 1)[0] + "\n" if status != "OK" else "OK\n")
unreal.SystemLibrary.quit_editor()
