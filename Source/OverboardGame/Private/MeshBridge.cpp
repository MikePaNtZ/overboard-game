// MeshBridge.cpp
//
// Pulls the standalone STL loader (repo-root mesh/, see mesh/README.md) into this module as a
// single translation unit, same pattern as WireBridge.cpp for wire/ -- one implementation, not a
// forked copy, compiled identically to what mesh/tests/test_stl_loader.cpp runs against.
#include "../../../mesh/StlLoader.cpp"
