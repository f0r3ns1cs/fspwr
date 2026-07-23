#pragma once

#include "winBase.h"

#include <functional>
#include <optional>
#include <string>

namespace pwr
{
	// Format: lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (no braces).
	std::wstring guidToString(const GUID& g);

	// Accepts with or without surrounding braces, any case. Returns nullopt on
	// malformed input.
	std::optional<GUID> guidFromString(std::wstring_view s);

	// UuidCreate wrapper (rpcrt4).
	GUID generateGuid();

	bool guidEqual(const GUID& a, const GUID& b);

	struct GuidLess {
		bool operator()(const GUID& a, const GUID& b) const noexcept;
	};

	struct GuidHash {
		size_t operator()(const GUID& g) const noexcept;
	};

	struct GuidEq {
		bool operator()(const GUID& a, const GUID& b) const noexcept {
			return guidEqual(a, b);
		}
	};

	// Well-known power GUIDs, defined here instead of via the SDK's initguid
	// machinery so there is exactly one definition site.
	namespace known
	{
		extern const GUID SchemeBalanced;        // 381b4222-f694-41f0-9685-ff5bb260df2e
		extern const GUID SchemeHighPerformance; // 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
		extern const GUID SchemePowerSaver;      // a1841308-3541-4fab-bc81-f71556f20b4a
		extern const GUID SchemeUltimate;        // e9a42b02-d5df-448d-aa00-03f14749eb61

		extern const GUID NoSubgroup;         // fea3413e-7e05-4911-9a71-700331f1c294
		extern const GUID SettingPersonality; // 245d8541-3943-4422-b025-13a784f679b7

	}

	bool isBuiltinScheme(const GUID& g);

}
