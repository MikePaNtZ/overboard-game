# wire/ — the OBW1/OBI1 wire layer, standalone

This is the wire spec from ADR-0010: packet layout, decode/encode, and the MuJoCo -> Unreal
coordinate transform, implemented as plain, UE-free C++17. It builds and runs with `clang++`
alone, deliberately, so the highest-risk and most reusable piece of W1 can be proven correct
before spending any time on the Unreal editor/build (see the pragmatics note in issue #162).

Once the Unreal project exists, `Source/OverboardGame/` includes and links these same files
rather than re-implementing the layout — do not fork this logic into a second copy.

## Layout
- `OverboardWire.h` / `.cpp` — packet constants, `FBoardState` (state in), `FInputPacket`
  (input out), byte-exact encode/decode. Fails loudly (returns false + an error string) on a
  short buffer, bad magic, or unknown `schema_version`; never guesses at a mismatched packet.
- `CoordinateTransform.h` / `.cpp` — the one `MuJoCoToUnreal()` function. Nothing else may do
  this conversion.
- `tests/test_wire.cpp` — encode/decode round-trips, magic/version/short-buffer rejection, and
  transform checks (identity, position, a commanded pure pitch, a commanded pure yaw).
- `tools/fake_sender.cpp` — stands in for the controls host: sends real OBW1 packets to
  `127.0.0.1:9601` (plus a `--bad-magic` mode to prove corrupt packets get dropped, not
  misparsed).
- `tools/test_receiver.cpp` — binds `127.0.0.1:9601` exactly like the real UE client will,
  decodes with the same `DecodeBoardState` the game uses, prints the transformed pose. Proves
  the receive+parse path end-to-end over a real socket without the editor.

## Build & run
```
cd wire
make test          # builds and runs tests/test_wire.cpp
make                # also builds fake_sender and test_receiver
./test_receiver &   # in one terminal
./fake_sender        # in another; sends 50 packets, ~1s
./fake_sender --bad-magic   # sends one corrupt packet, watch test_receiver print DROP: ...
```

## What is NOT proven here
The transform's handedness/sign choices are implemented exactly per the ADR-0010 baseline and
unit-tested for the *values* the formula produces (see `Test_TransformPurePitch` /
`Test_TransformPureYaw` in `tests/test_wire.cpp`), but **not visually confirmed** — that needs
the Unreal editor open with a board actor on screen, commanding a known pure pitch and pure yaw
and watching which way it turns. Do that before trusting this for anything beyond "the math the
spec described is what got implemented."
