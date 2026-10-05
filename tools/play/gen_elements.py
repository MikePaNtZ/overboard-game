#!/usr/bin/env python3
"""Write the game-element layout for a MuJoCo course.

The elements are read-only: they read the board pose and flags and score the ride. They put no
force on the board (sim-host owns all physics). Positions are in course coordinates:
s = distance along the street from the course start (MuJoCo x = start_x - s), y = MuJoCo lateral
(+Y is the rider's right... see ABoardActor's frame note: Unreal Y = -100 * MuJoCo y).

Usage: tools/play/gen_elements.py <course_dir> [out.json]
Default out: tools/play/elements/<course name>.json
"""
import json
import os
import sys

# city_hill (Mike, 2026-10-04: four flags, then a top-speed run): run-in 0-12 m, 15 % descent 12-82 m, flat 82-112 m, 12 % climb 112-167 m, crest 167-182 m.
CITY_HILL = [
    {"type": "gate", "id": "start", "label": "START", "s": 8.0},
    # Slalom on the upper descent: pass each flag on its OUTER side (|y| greater than the flag's |y|).
    # |y| = 1.8 m keeps the poles out of the centre band |y| < 1.6 m (render track rule).
    {"type": "flag", "id": "flag1", "s": 20.0, "y": 1.8},
    {"type": "flag", "id": "flag2", "s": 30.0, "y": -1.8},
    {"type": "flag", "id": "flag3", "s": 40.0, "y": 1.8},
    {"type": "flag", "id": "flag4", "s": 50.0, "y": -1.8},
    # Lower descent: a straight speed run; the peak speed shows on the HUD.
    {"type": "speed_trap", "id": "speed", "label": "SPEED TRAP", "s0": 54.0, "s1": 76.0, "bonus_speed_mps": 7.0},
    # On the flat: stop inside the box. A tail-down stop (pitch nose-up) scores more.
    {"type": "stop_box", "id": "stop", "label": "STOP BOX", "s0": 90.0, "s1": 97.0, "half_width": 3.0,
     "stop_speed_mps": 0.3, "tail_pitch_rad": 0.25},
    {"type": "slow_zone", "id": "slow", "label": "SLOW 3 m/s", "s0": 100.0, "s1": 112.0, "max_speed_mps": 3.0},
    {"type": "gate", "id": "split", "label": "SPLIT", "s": 106.0},
    {"type": "no_buzz", "id": "climb", "label": "NO-BUZZ CLIMB", "s0": 114.0, "s1": 166.0},
    {"type": "gate", "id": "finish", "label": "FINISH", "s": 172.0},
]

SCORES = {
    "flag": 100,
    "speed_trap": 200,
    "stop": 300,
    "tail_stop_bonus": 200,
    "slow_clean": 200,
    "slow_penalty": -100,
    "no_buzz": 300,
    "finish": 500,
}


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    course_dir = sys.argv[1].rstrip("/")
    name = os.path.basename(course_dir)
    if name != "city_hill":
        sys.exit(f"no element layout for course '{name}' yet")
    course = json.load(open(os.path.join(course_dir, "course.json")))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "elements", f"{name}.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    path = course["path"]
    doc = {
        "course": name,
        "start_x_m": path["start_x_m"],
        "lane_half_width_m": path["width_m"] / 2 - path["lane_margin_m"],
        "street_half_width_m": path["width_m"] / 2,
        "profile": {"s_m": course["profile"]["s_m"], "z_m": course["profile"]["z_m"]},
        "scores": SCORES,
        "elements": CITY_HILL,
    }
    with open(out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"wrote {out}: {len(CITY_HILL)} elements")


if __name__ == "__main__":
    main()
