#include "attributes.h"

#include "powerScheme.h"

#ifndef POWER_ATTRIBUTE_HIDE
#define POWER_ATTRIBUTE_HIDE 0x00000001
#endif

namespace pwr
{
	namespace
	{
		std::wstring ctx(const SettingRef& ref) {
			return L"subgroup=" + guidToString(ref.subgroup)
			     + L" setting=" + guidToString(ref.id);
		}

		// Re-applies whatever scheme is already active, which is how the power service is told to re-read.
		void nudgePowerService() {
			SchemeManager mgr;
			if (const PowerResult<GUID> active = mgr.activeScheme())
				(void)mgr.setActiveScheme(*active);
		}

		AttributeBatch writeAll(const std::vector<SettingRef>& refs, bool hidden) {
			AttributeBatch out;
			for (const SettingRef& ref : refs) {
				const DWORD current =
					::PowerReadSettingAttributes(&ref.subgroup, &ref.id);
				const DWORD next = hidden ? (current | POWER_ATTRIBUTE_HIDE)
				                          : (current & ~DWORD(POWER_ATTRIBUTE_HIDE));
				if (next == current)
					continue;   // already where it needs to be

				const DWORD rc =
					::PowerWriteSettingAttributes(&ref.subgroup, &ref.id, next);
				if (rc != ERROR_SUCCESS) {
					++out.failed;
					if (!out.firstError)
						out.firstError = makeError(rc, L"PowerWriteSettingAttributes",
						                           ctx(ref));
					continue;
				}
				++out.changed;
			}
			if (out.changed > 0)
				nudgePowerService();
			return out;
		}
	}

	PowerResult<void> setHidden(const SettingRef& ref, bool hidden)
	{
		const DWORD current = ::PowerReadSettingAttributes(&ref.subgroup, &ref.id);
		const DWORD next = hidden ? (current | POWER_ATTRIBUTE_HIDE)
		                          : (current & ~DWORD(POWER_ATTRIBUTE_HIDE));
		if (next == current)
			return {};

		const DWORD rc = ::PowerWriteSettingAttributes(&ref.subgroup, &ref.id, next);
		if (rc != ERROR_SUCCESS)
			return errResult(rc, L"PowerWriteSettingAttributes", ctx(ref));

		nudgePowerService();
		return {};
	}

	AttributeBatch unhideAll(const std::vector<SettingRef>& refs)
	{
		return writeAll(refs, false);
	}

	AttributeBatch hideAll(const std::vector<SettingRef>& refs)
	{
		return writeAll(refs, true);
	}
}
