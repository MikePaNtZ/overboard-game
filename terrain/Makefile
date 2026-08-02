# Standalone build for the authored-world terrain envelope -- no Unreal/UBT involved.
# Same pattern as ../wire/Makefile and ../mesh/Makefile, and for the same reason: this has to run
# on every push, in CI, in seconds. A gate that needs a 1.5 GB Fab pack and an editor to run is a
# gate that runs nowhere.
CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g

SRCS = TerrainDecl.cpp SurfaceCheck.cpp ../mesh/StlLoader.cpp
HDRS = TerrainDecl.h SurfaceCheck.h TerrainLimits.h ../mesh/StlLoader.h

.PHONY: all test check strict clean

all: test_terrain terraincheck

test_terrain: tests/test_terrain.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) tests/test_terrain.cpp $(SRCS) -o test_terrain

terraincheck: tools/terraincheck.cpp $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) tools/terraincheck.cpp $(SRCS) -o terraincheck

# `make test` proves the RULES fire. `make check` applies them to the levels this repo ships.
# CI runs both -- a validator nobody points at the assets is not a gate.
test: test_terrain
	./test_terrain

check: terraincheck
	./terraincheck --repo-root ..

# What a public build must run. Fails on surfaces that are declared rather than measured.
strict: terraincheck
	./terraincheck --repo-root .. --strict

clean:
	rm -f test_terrain terraincheck
	rm -rf *.dSYM
