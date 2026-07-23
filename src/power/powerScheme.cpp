#include "powerScheme.h"

#include "powerBuffers.h"

#pragma comment(lib, "Advapi32.lib")

namespace pwr
{
	namespace
	{
		// PowerGetActiveScheme returns a GUID allocated with LocalAlloc.
		struct LocalGuidHolder {
			GUID* p = nullptr;
			~LocalGuidHolder() {
				if (p) ::LocalFree(p);
			}
		};

		std::wstring schemeContext(const GUID& scheme) {
			return L"scheme=" + guidToString(scheme);
		}

	}

	PowerResult<std::vector<GUID>> SchemeManager::enumerateSchemeGuids() {
		std::vector<GUID> out;
		auto contains = [&](const GUID& g) {
			for (const auto& x : out)
				if (guidEqual(x, g)) return true;
			return false;
		};

		for (ULONG i = 0;; ++i) {
			GUID g{};
			DWORD size = sizeof(g);
			const DWORD rc = ::PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i,
			                                  reinterpret_cast<UCHAR*>(&g), &size);
			if (rc == ERROR_NO_MORE_ITEMS) break;
			if (rc != ERROR_SUCCESS) {
				return errResult(rc, L"PowerEnumerate(ACCESS_SCHEME)", L"index=" + std::to_wstring(i));
			}
			if (!contains(g)) out.push_back(g);
		}

		HKEY key = nullptr;
		if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		                    L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes", 0, KEY_READ,
		                    &key) == ERROR_SUCCESS) {
			wchar_t name[64];
			for (DWORD i = 0;; ++i) {
				DWORD len = ARRAYSIZE(name);
				if (::RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
					break;
				if (auto g = guidFromString(name); g && !contains(*g)) out.push_back(*g);
			}
			::RegCloseKey(key);
		}
		return out;
	}

	PowerResult<std::vector<SchemeInfo>> SchemeManager::enumerateSchemes() {
		auto guids = enumerateSchemeGuids();
		if (!guids) return Unexpected<PowerError>{std::move(guids).error()};

		GUID active{};
		if (auto a = activeScheme()) {
			active = *a;
		}

		std::vector<SchemeInfo> out;
		out.reserve(guids->size());
		for (const GUID& g : *guids) {
			SchemeInfo info;
			info.id = g;
			info.isBuiltin = isBuiltinScheme(g);
			info.isActive = guidEqual(g, active);
			if (auto name = friendlyName(g)) {
				info.name = std::move(*name);
			}
			if (auto desc = description(g)) {
				info.description = std::move(*desc);
			}
			out.push_back(std::move(info));
		}
		return out;
	}

	PowerResult<GUID> SchemeManager::activeScheme() {
		LocalGuidHolder holder;
		const DWORD rc = ::PowerGetActiveScheme(nullptr, &holder.p);
		if (rc != ERROR_SUCCESS || holder.p == nullptr) {
			return errResult(rc, L"PowerGetActiveScheme");
		}
		return *holder.p;
	}

	PowerResult<void> SchemeManager::setActiveScheme(const GUID& scheme) {
		const DWORD rc = ::PowerSetActiveScheme(nullptr, &scheme);
		if (rc != ERROR_SUCCESS) {
			return errResult(rc, L"PowerSetActiveScheme", schemeContext(scheme));
		}
		return {};
	}

	PowerResult<std::wstring> SchemeManager::friendlyName(const GUID& scheme) {
		return readPowerString(L"PowerReadFriendlyName", schemeContext(scheme), [&](UCHAR* buf, DWORD* size) {
			return ::PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr, buf, size);
		});
	}

	PowerResult<std::wstring> SchemeManager::description(const GUID& scheme) {
		return readPowerString(L"PowerReadDescription", schemeContext(scheme), [&](UCHAR* buf, DWORD* size) {
			return ::PowerReadDescription(nullptr, &scheme, nullptr, nullptr, buf, size);
		});
	}

	PowerResult<void> SchemeManager::setFriendlyName(const GUID& scheme, const std::wstring& name) {
		const DWORD rc = ::PowerWriteFriendlyName(nullptr, &scheme, nullptr, nullptr, powerStringPtr(name),
		                                          powerStringBytes(name));
		if (rc != ERROR_SUCCESS) {
			return errResult(rc, L"PowerWriteFriendlyName", schemeContext(scheme));
		}
		return {};
	}

	PowerResult<void> SchemeManager::setDescription(const GUID& scheme, const std::wstring& description) {
		const DWORD rc = ::PowerWriteDescription(nullptr, &scheme, nullptr, nullptr,
		                                         powerStringPtr(description), powerStringBytes(description));
		if (rc != ERROR_SUCCESS) {
			return errResult(rc, L"PowerWriteDescription", schemeContext(scheme));
		}
		return {};
	}

	PowerResult<SchemeManager::DuplicateOutcome> SchemeManager::duplicateScheme(const GUID& source,
	                                                                            const GUID& dest) {
		// Passing a non-null destination pointer makes PowerDuplicateScheme clone
		// into that GUID instead of allocating a fresh one.
		GUID destCopy = dest;
		GUID* destPtr = &destCopy;
		const DWORD rc = ::PowerDuplicateScheme(nullptr, &source, &destPtr);
		if (rc == ERROR_ALREADY_EXISTS || rc == ERROR_OBJECT_ALREADY_EXISTS) {
			return DuplicateOutcome{dest, true};
		}
		if (rc != ERROR_SUCCESS) {
			return errResult(rc, L"PowerDuplicateScheme",
			                 L"source=" + guidToString(source) + L" dest=" + guidToString(dest));
		}
		return DuplicateOutcome{dest, false};
	}

	PowerResult<void> SchemeManager::deleteScheme(const GUID& scheme) {
		if (auto active = activeScheme(); active && guidEqual(*active, scheme)) {
			return errResult(ERROR_BUSY, L"deleteScheme",
			                 L"refusing to delete the active scheme " + guidToString(scheme) +
			                     L"; switch the active scheme first");
		}
		const DWORD rc = ::PowerDeleteScheme(nullptr, &scheme);
		if (rc != ERROR_SUCCESS) {
			return errResult(rc, L"PowerDeleteScheme", schemeContext(scheme));
		}
		return {};
	}

	bool SchemeManager::schemeExists(const GUID& scheme) {
		return friendlyName(scheme).hasValue();
	}

}
