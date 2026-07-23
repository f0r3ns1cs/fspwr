#pragma once

#include "guidUtil.h"
#include "powerError.h"
#include "powerSettings.h"

#include <optional>
#include <string>
#include <vector>

namespace pwr
{
	struct PendingChange {
		GUID subgroup{};
		GUID setting{};
		std::optional<DWORD> acOld, acNew, dcOld, dcNew;

		bool touchesAc() const {
			return acNew.has_value();
		}
		bool touchesDc() const {
			return dcNew.has_value();
		}
	};

	// Edits keyed by (subgroup, setting). Re-staging a rail keeps the original old
	// value but updates the new one; staging back to the old value drops that
	// rail; an entry with no staged rails is removed.
	class PendingChangeSet {
	  public:
		void stageAc(const GUID& subgroup, const GUID& setting, std::optional<DWORD> oldValue,
		             DWORD newValue);
		void stageDc(const GUID& subgroup, const GUID& setting, std::optional<DWORD> oldValue,
		             DWORD newValue);
		void unstage(const GUID& subgroup, const GUID& setting);
		void clear();

		bool empty() const {
			return changes_.empty();
		}
		size_t count() const {
			return changes_.size();
		}
		const std::vector<PendingChange>& changes() const {
			return changes_;
		}
		const PendingChange* find(const GUID& subgroup, const GUID& setting) const;

	  private:
		enum class Rail { Ac, Dc };
		void stage(Rail rail, const GUID& subgroup, const GUID& setting, std::optional<DWORD> oldValue,
		           DWORD newValue);

		std::vector<PendingChange> changes_;
	};

	// Returns an error message if value is out of range / not a valid enum index,
	// or nullopt if it is acceptable. Out-of-range writes can appear to succeed
	// and then misbehave, so callers validate first. Opaque settings accept any
	// raw index.
	std::optional<std::wstring> validateValue(const SettingDescriptor& desc, DWORD value);

	struct CommitFailure {
		PendingChange change;
		PowerError error;
	};

	struct CommitReport {
		size_t appliedWrites = 0;
		bool reactivated = false;
		std::vector<CommitFailure> failures;
		bool allSucceeded() const {
			return failures.empty();
		}
	};

	class ValueAccessor {
	  public:
		PowerResult<void> writeAcIndex(const GUID& scheme, const GUID& subgroup, const GUID& setting,
		                               DWORD value);
		PowerResult<void> writeDcIndex(const GUID& scheme, const GUID& subgroup, const GUID& setting,
		                               DWORD value);

		// Applies all staged writes, then re-activates scheme if it is active so
		// the writes take effect. A failed write is recorded in the report and
		// does not stop the remaining writes.
		PowerResult<CommitReport> commit(const GUID& scheme, const PendingChangeSet& changes);
	};

}
