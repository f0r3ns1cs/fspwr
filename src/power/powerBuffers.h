#pragma once

#include "powerError.h"

#include <string>
#include <vector>

namespace pwr
{
	// fn signature: DWORD fn(UCHAR* buffer, DWORD* size). nullptr buffer queries
	// the required size. Returns the raw byte buffer (may be empty).
	template <typename F>
	PowerResult<std::vector<UCHAR>> readPowerBuffer(const wchar_t* op, std::wstring context, F&& fn) {
		DWORD size = 0;
		DWORD rc = fn(nullptr, &size);
		// Size queries return ERROR_SUCCESS or ERROR_MORE_DATA depending on the API.
		if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) {
			return errResult(rc, op, std::move(context));
		}
		std::vector<UCHAR> buf(size);
		if (size == 0) return buf;
		rc = fn(buf.data(), &size);
		if (rc != ERROR_SUCCESS) {
			return errResult(rc, op, std::move(context));
		}
		buf.resize(size);
		return buf;
	}

	// Interprets a power API byte buffer as a null-terminated UTF-16 string.
	inline std::wstring wideFromBuffer(const std::vector<UCHAR>& buf) {
		if (buf.size() < sizeof(wchar_t)) return {};
		const wchar_t* p = reinterpret_cast<const wchar_t*>(buf.data());
		size_t chars = buf.size() / sizeof(wchar_t);
		while (chars > 0 && p[chars - 1] == L'\0')
			--chars;
		return std::wstring(p, chars);
	}

	template <typename F>
	PowerResult<std::wstring> readPowerString(const wchar_t* op, std::wstring context, F&& fn) {
		auto buf = readPowerBuffer(op, std::move(context), std::forward<F>(fn));
		if (!buf) return Unexpected<PowerError>{std::move(buf).error()};
		return wideFromBuffer(*buf);
	}

	// Byte count INCLUDING the null terminator, as PowerWriteFriendlyName and
	// PowerWriteDescription require.
	inline DWORD powerStringBytes(const std::wstring& s) {
		return static_cast<DWORD>((s.size() + 1) * sizeof(wchar_t));
	}

	inline UCHAR* powerStringPtr(const std::wstring& s) {
		return reinterpret_cast<UCHAR*>(const_cast<wchar_t*>(s.c_str()));
	}

}
