#!/bin/bash
# Fetch the public source data for Level 2 (the Embarcadero cruise loop). Not committed.
#   OpenStreetMap extract: (c) OpenStreetMap contributors, ODbL 1.0 (https://www.openstreetmap.org/copyright)
#   USGS 3DEP 1 m DEM, tile x55y419, project CA_SanFrancisco_B23 (public domain)
set -euo pipefail
DATA=${OB_LEVELS_DATA:-$HOME/projects/overboard-viz/out/carve-lab/data/sources/embarcadero}
mkdir -p "$DATA"
[ -s "$DATA/embarcadero.osm" ] || curl -sS -m 180 -A "overboard-levels/0.1" -o "$DATA/embarcadero.osm" \
  "https://api.openstreetmap.org/api/0.6/map?bbox=-122.4000,37.7735,-122.3845,37.7920"
[ -s "$DATA/dem_x55y419.tif" ] || curl -sS -m 900 -o "$DATA/dem_x55y419.tif" \
  "https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/1m/Projects/CA_SanFrancisco_B23/TIFF/USGS_1M_10_x55y419_CA_SanFrancisco_B23.tif"
ls -la "$DATA"
