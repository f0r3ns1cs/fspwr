#pragma once

#include "guidUtil.h"
#include "powerError.h"

#include <optional>
#include <vector>

namespace pwr
{
	struct SettingRef {
		GUID subgroup{};
		GUID id{};
	};

	// Clears or sets POWER_ATTRIBUTE_HIDE on one setting.
	PowerResult<void> setHidden(const SettingRef& ref, bool hidden);

	struct AttributeBatch {
		int changed = 0;
		int failed  = 0;
		std::optional<PowerError> firstError;
	};

	// Clears the hide bit on every ref given, then re-applies the active scheme.
	AttributeBatch unhideAll(const std::vector<SettingRef>& refs);

	// Restores the hide bit on every ref given.
	AttributeBatch hideAll(const std::vector<SettingRef>& refs);
}
