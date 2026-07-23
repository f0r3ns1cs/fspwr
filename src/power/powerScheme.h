#pragma once

// Enumerate, duplicate, rename, delete and activate power schemes.

#include "guidUtil.h"
#include "powerError.h"

#include <string>
#include <vector>

namespace pwr
{
	struct SchemeInfo {
		GUID id{};
		std::wstring name;
		std::wstring description;
		bool isActive = false;
		bool isBuiltin = false;
		bool isOurs = false;
	};

	class SchemeManager {
	  public:
		PowerResult<std::vector<GUID>> enumerateSchemeGuids();

		// Names, descriptions, active/builtin flags. Partial failures (e.g. a
		// scheme with no description) degrade to empty strings, never abort.
		PowerResult<std::vector<SchemeInfo>> enumerateSchemes();

		PowerResult<GUID> activeScheme();
		PowerResult<void> setActiveScheme(const GUID& scheme);

		PowerResult<std::wstring> friendlyName(const GUID& scheme);
		PowerResult<std::wstring> description(const GUID& scheme);
		PowerResult<void> setFriendlyName(const GUID& scheme, const std::wstring& name);
		PowerResult<void> setDescription(const GUID& scheme, const std::wstring& description);

		struct DuplicateOutcome {
			GUID id{};
			bool alreadyExisted = false; // ERROR_ALREADY_EXISTS: dest already exists
		};

		// Clone source into the caller-supplied GUID dest. If dest already exists
		// this returns success with alreadyExisted set, so re-running the tool
		// updates the plan in place instead of failing.
		PowerResult<DuplicateOutcome> duplicateScheme(const GUID& source, const GUID& dest);

		// Returns ERROR_BUSY for the active scheme; switch away from it first.
		PowerResult<void> deleteScheme(const GUID& scheme);

		bool schemeExists(const GUID& scheme);
	};
}
