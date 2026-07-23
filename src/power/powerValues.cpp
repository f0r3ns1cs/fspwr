#include "powerValues.h"

#include "powerScheme.h"

namespace pwr
{
	namespace
	{
		std::wstring ctx(const GUID& scheme, const GUID& subgroup, const GUID& setting) {
			return L"scheme=" + guidToString(scheme) + L" subgroup=" + guidToString(subgroup) + L" setting=" +
			       guidToString(setting);
		}

	}

	void PendingChangeSet::stage(Rail rail, const GUID& subgroup, const GUID& setting,
	                             std::optional<DWORD> oldValue, DWORD newValue) {
		PendingChange* entry = nullptr;
		for (auto& c : changes_) {
			if (guidEqual(c.subgroup, subgroup) && guidEqual(c.setting, setting)) {
				entry = &c;
				break;
			}
		}
		if (!entry) {
			changes_.push_back(PendingChange{subgroup, setting, {}, {}, {}, {}});
			entry = &changes_.back();
		}

		auto& oldSlot = rail == Rail::Ac ? entry->acOld : entry->dcOld;
		auto& newSlot = rail == Rail::Ac ? entry->acNew : entry->dcNew;

		if (!newSlot.has_value()) {
			oldSlot = oldValue;
		}
		if (oldSlot.has_value() && *oldSlot == newValue) {
			newSlot.reset();
			oldSlot.reset();
		} else {
			newSlot = newValue;
		}

		if (!entry->touchesAc() && !entry->touchesDc()) {
			unstage(subgroup, setting);
		}
	}

	void PendingChangeSet::stageAc(const GUID& subgroup, const GUID& setting, std::optional<DWORD> oldValue,
	                               DWORD newValue) {
		stage(Rail::Ac, subgroup, setting, oldValue, newValue);
	}

	void PendingChangeSet::stageDc(const GUID& subgroup, const GUID& setting, std::optional<DWORD> oldValue,
	                               DWORD newValue) {
		stage(Rail::Dc, subgroup, setting, oldValue, newValue);
	}

	void PendingChangeSet::unstage(const GUID& subgroup, const GUID& setting) {
		for (auto it = changes_.begin(); it != changes_.end(); ++it) {
			if (guidEqual(it->subgroup, subgroup) && guidEqual(it->setting, setting)) {
				changes_.erase(it);
				return;
			}
		}
	}

	void PendingChangeSet::clear() {
		changes_.clear();
	}

	const PendingChange* PendingChangeSet::find(const GUID& subgroup, const GUID& setting) const {
		for (const auto& c : changes_) {
			if (guidEqual(c.subgroup, subgroup) && guidEqual(c.setting, setting)) {
				return &c;
			}
		}
		return nullptr;
	}

	std::optional<std::wstring> validateValue(const SettingDescriptor& desc, DWORD value) {
		switch (desc.kind) {
		case SettingKind::Range: {
			if (value < desc.min || value > desc.max) {
				return L"value " + std::to_wstring(value) + L" is outside the range " +
				       std::to_wstring(desc.min) + L" to " + std::to_wstring(desc.max);
			}
			if (desc.increment > 1 && (value - desc.min) % desc.increment != 0) {
				return L"value " + std::to_wstring(value) + L" does not align to the increment of " +
				       std::to_wstring(desc.increment);
			}
			return std::nullopt;
		}
		case SettingKind::Enum: {
			for (const auto& pv : desc.possibleValues) {
				if (pv.index == value) return std::nullopt;
			}
			return L"value " + std::to_wstring(value) + L" is not in the list of possible values for \"" +
			       desc.name + L"\"";
		}
		case SettingKind::Opaque: return std::nullopt;
		}
		return std::nullopt;
	}

	PowerResult<void> ValueAccessor::writeAcIndex(const GUID& scheme, const GUID& subgroup,
	                                              const GUID& setting, DWORD value) {
		const DWORD rc = ::PowerWriteACValueIndex(nullptr, &scheme, &subgroup, &setting, value);
		if (rc != ERROR_SUCCESS)
			return errResult(rc, L"PowerWriteACValueIndex", ctx(scheme, subgroup, setting));
		return {};
	}

	PowerResult<void> ValueAccessor::writeDcIndex(const GUID& scheme, const GUID& subgroup,
	                                              const GUID& setting, DWORD value) {
		const DWORD rc = ::PowerWriteDCValueIndex(nullptr, &scheme, &subgroup, &setting, value);
		if (rc != ERROR_SUCCESS)
			return errResult(rc, L"PowerWriteDCValueIndex", ctx(scheme, subgroup, setting));
		return {};
	}

	PowerResult<CommitReport> ValueAccessor::commit(const GUID& scheme, const PendingChangeSet& changes) {
		CommitReport report;

		for (const PendingChange& c : changes.changes()) {
			if (c.acNew) {
				auto r = writeAcIndex(scheme, c.subgroup, c.setting, *c.acNew);
				if (r) {
					++report.appliedWrites;
				} else {
					report.failures.push_back(CommitFailure{c, r.error()});
				}
			}
			if (c.dcNew) {
				auto r = writeDcIndex(scheme, c.subgroup, c.setting, *c.dcNew);
				if (r) {
					++report.appliedWrites;
				} else {
					report.failures.push_back(CommitFailure{c, r.error()});
				}
			}
		}

		// Index writes do not take effect until the scheme is re-activated, so if
		// we edited the active scheme, activate it again.
		SchemeManager schemes;
		if (auto active = schemes.activeScheme(); active && guidEqual(*active, scheme)) {
			if (report.appliedWrites > 0) {
				auto r = schemes.setActiveScheme(scheme);
				if (!r) return Unexpected<PowerError>{r.error()};
				report.reactivated = true;
			}
		}
		return report;
	}

}
