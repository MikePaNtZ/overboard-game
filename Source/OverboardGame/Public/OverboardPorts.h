// OverboardPorts -- the UDP ports of the sim-host <-> game wire, from one base.
//
// -ObPortBase=N (default 9600): StateOut on N+1, InputIn on N+2, the HUD packet on N+3, objects on
// N+4. Two runs on one Mac (live play, a test, another session's demo) must use different bases,
// or they read each other's packets (2026-10-06: a demo and a levels test collided on 9601/9602).
// The scripts pass the same base to sim-host as PORT_BASE (tools/play/run_sim.sh).

#pragma once

#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace OverboardPorts
{
	inline int32 Base()
	{
		static const int32 Cached = []()
		{
			int32 Value = 9600;
			FParse::Value(FCommandLine::Get(), TEXT("ObPortBase="), Value);
			return Value;
		}();
		return Cached;
	}
	inline int32 State() { return Base() + 1; }
	inline int32 Input() { return Base() + 2; }
	inline int32 Hud() { return Base() + 3; }
	inline int32 Objects() { return Base() + 4; }
}
