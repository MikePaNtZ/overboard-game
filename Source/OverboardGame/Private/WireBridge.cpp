// WireBridge.cpp
//
// Pulls the standalone wire layer (repo-root wire/, see wire/README.md) into this module as a
// single translation unit, so UnrealBuildTool compiles the exact same implementation the
// standalone tests run against -- no forked copy. This is the one place that does this; every
// other file in this module just #include "OverboardWire.h" / "CoordinateTransform.h" (resolved
// via the PublicIncludePaths entry in OverboardGame.Build.cs) and links against the symbols
// compiled here.
#include "../../../wire/OverboardWire.cpp"
#include "../../../wire/CoordinateTransform.cpp"
