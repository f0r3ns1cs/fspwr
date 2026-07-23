#include "accessCheck.h"

namespace pwr
{
	namespace
	{
		PolicyState classify(DWORD rc) {
			switch (rc) {
			case ERROR_SUCCESS: return PolicyState::Allowed;
			case ERROR_ACCESS_DISABLED_BY_POLICY: return PolicyState::DisabledByPolicy;
			case ERROR_ACCESS_DENIED: return PolicyState::AccessDenied;
			default: return PolicyState::Unknown;
			}
		}

	}

	PolicyState PolicyChecker::createSchemeState() {
		return classify(::PowerSettingAccessCheck(ACCESS_CREATE_SCHEME, nullptr));
	}

	PolicyState PolicyChecker::activateSchemeState() {
		return classify(::PowerSettingAccessCheck(ACCESS_ACTIVE_SCHEME, nullptr));
	}

	PolicyState PolicyChecker::settingWriteState(const GUID& setting, PowerRail rail) {
		const POWER_DATA_ACCESSOR accessor =
		    rail == PowerRail::Ac ? ACCESS_AC_POWER_SETTING_INDEX : ACCESS_DC_POWER_SETTING_INDEX;
		return classify(::PowerSettingAccessCheck(accessor, &setting));
	}

	const wchar_t* PolicyChecker::describe(PolicyState s) {
		switch (s) {
		case PolicyState::Allowed: return L"allowed";
		case PolicyState::DisabledByPolicy: return L"locked by group policy";
		case PolicyState::AccessDenied: return L"access denied (elevation required)";
		case PolicyState::Unknown: return L"unknown";
		}
		return L"unknown";
	}

}
