#include "platform.h"

namespace pwr
{
	bool isModernStandby() {
		SYSTEM_POWER_CAPABILITIES spc{};
		// Returns NTSTATUS; 0 == success. (LONG avoids pulling in winternl.h.)
		const LONG status = ::CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &spc, sizeof(spc));
		return status == 0 && spc.AoAc != FALSE;
	}

	DWORD osBuildNumber() {
		// RtlGetVersion gives the true build number regardless of manifest.
		using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
		if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
			if (auto fn = reinterpret_cast<RtlGetVersionFn>(
			        reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")))) {
				RTL_OSVERSIONINFOW info{};
				info.dwOSVersionInfoSize = sizeof(info);
				if (fn(&info) == 0) {
					return info.dwBuildNumber;
				}
			}
		}
		return 0;
	}

	bool isElevated() {
		HANDLE token = nullptr;
		if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
			return false;
		}
		TOKEN_ELEVATION elevation{};
		DWORD size = 0;
		const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
		::CloseHandle(token);
		return ok && elevation.TokenIsElevated != 0;
	}

}
