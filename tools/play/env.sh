# Source this file. It sets the paths that sim-host, wire-probe and send-input need.
CONTROLS=${CONTROLS:-$HOME/projects/overboard-play-controls}
COURSE=${COURSE:-$HOME/projects/overboard-viz/out/carve-lab/data/courses/city_hill}
PY=${PY:-$HOME/projects/overboard/.venv/bin/python}
export PATH="$HOME/.cargo/bin:/usr/sbin:/sbin:$PATH"
export MUJOCO_DIR=$($PY -c "import mujoco,os;print(os.path.dirname(mujoco.__file__))")
export DYLD_FRAMEWORK_PATH="$MUJOCO_DIR/.dylibs:$MUJOCO_DIR"
export DYLD_LIBRARY_PATH=$(ls -d "$CONTROLS"/target/release/build/plant-mujoco-*/out | head -1)
BIN="$CONTROLS/target/release"
