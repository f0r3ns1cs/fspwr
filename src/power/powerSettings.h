#pragma once

#include "guidUtil.h"
#include "powerError.h"

#include <optional>
#include <string>
#include <vector>

namespace pwr
{
	enum class SettingKind { Range, Enum, Opaque };

	struct PossibleValue {
		DWORD index = 0;
		std::wstring name;
		std::wstring description;
	};

	struct SettingDescriptor {
		GUID subgroup{};
		GUID id{};
		std::wstring name;
		std::wstring description;
		std::wstring units;
		SettingKind kind = SettingKind::Opaque;
		DWORD min = 0;
		DWORD max = 0;
		DWORD increment = 1;
		std::vector<PossibleValue> possibleValues;
		bool hidden = false; // POWER_ATTRIBUTE_HIDE; display only, never written
		bool acLockedByPolicy = false;
		bool dcLockedByPolicy = false;
		bool metadataIncomplete = false; // no range or enum metadata; raw index only

		// Formats a raw index for display: the enum value name, or number + units.
		std::wstring renderValue(std::optional<DWORD> v) const;
	};

	struct SettingValues {
		std::optional<DWORD> ac, dc, acDefault, dcDefault;
	};

	struct SettingEntry {
		SettingDescriptor desc;
		SettingValues values;
	};

	struct SubgroupInfo {
		GUID id{};
		std::wstring name;
		std::wstring description;
	};

	struct SchemeEnumeration {
		GUID scheme{};
		GUID defaultsScheme{};               // personality GUID used for default-index reads
		std::vector<SubgroupInfo> subgroups; // NO_SUBGROUP first
		std::vector<SettingEntry> settings;
		std::vector<PowerError> warnings; // per-setting read failures; not fatal
	};

	class SettingsEnumerator {
	  public:
		// knownBase: the scheme this plan was duplicated from, if known; used as a
		// defaults fallback when the personality read fails.
		PowerResult<SchemeEnumeration> enumerateScheme(const GUID& scheme,
		                                               std::optional<GUID> knownBase = std::nullopt);

		// PowerReadAC/DCDefaultIndex want the scheme personality GUID, not the
		// scheme GUID. This resolves the right one.
		GUID resolveDefaultsScheme(const GUID& scheme, std::optional<GUID> knownBase);
	};

}
