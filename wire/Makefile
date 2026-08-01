# Standalone build for the wire layer -- no Unreal/UBT involved. See ../README.md (root) and
# ./README.md for what this proves and why it's built before the UE project.
CXX ?= clang++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g

WIRE_SRCS = OverboardWire.cpp CoordinateTransform.cpp

.PHONY: all test clean

all: test_wire fake_sender test_receiver

test_wire: tests/test_wire.cpp $(WIRE_SRCS) OverboardWire.h CoordinateTransform.h
	$(CXX) $(CXXFLAGS) tests/test_wire.cpp $(WIRE_SRCS) -o test_wire

fake_sender: tools/fake_sender.cpp $(WIRE_SRCS) OverboardWire.h
	$(CXX) $(CXXFLAGS) tools/fake_sender.cpp $(WIRE_SRCS) -o fake_sender

test_receiver: tools/test_receiver.cpp $(WIRE_SRCS) OverboardWire.h CoordinateTransform.h
	$(CXX) $(CXXFLAGS) tools/test_receiver.cpp $(WIRE_SRCS) -o test_receiver

test: test_wire
	./test_wire

clean:
	rm -f test_wire fake_sender test_receiver
	rm -rf *.dSYM
