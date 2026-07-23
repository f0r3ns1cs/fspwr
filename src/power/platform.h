#pragma once

// Platform probes: Modern Standby, OS build, token elevation.

#include "winBase.h"

namespace pwr
{
	// Reads SYSTEM_POWER_CAPABILITIES.AoAc. The CsEnabled registry value is not
	// reliable on recent builds, so we do not use it.
	bool isModernStandby();

	DWORD osBuildNumber();

	bool isElevated();

}
