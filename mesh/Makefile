# Standalone build for the STL loader -- no Unreal/UBT involved. Same pattern as ../wire/Makefile.
CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g

.PHONY: all test clean

all: test_stl_loader

test_stl_loader: tests/test_stl_loader.cpp StlLoader.cpp StlLoader.h
	$(CXX) $(CXXFLAGS) tests/test_stl_loader.cpp StlLoader.cpp -o test_stl_loader

test: test_stl_loader
	./test_stl_loader

clean:
	rm -f test_stl_loader
	rm -rf *.dSYM
