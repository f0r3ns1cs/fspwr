#pragma once

#include "guidUtil.h"
#include "powerError.h"

namespace pwr
{
	enum class PowerRail { Ac, Dc };

	enum class PolicyState {
		Allowed,
		DisabledByPolicy, // ERROR_ACCESS_DISABLED_BY_POLICY
		AccessDenied,     // ERROR_ACCESS_DENIED (e.g. not elevated)
		Unknown,          // other error; treat as allowed but log
	};

	class PolicyChecker {
	  public:
		PolicyState createSchemeState();
		PolicyState activateSchemeState();
		PolicyState settingWriteState(const GUID& setting, PowerRail rail);

		static const wchar_t* describe(PolicyState s);
	};
}
