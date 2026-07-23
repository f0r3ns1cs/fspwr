#pragma once

#include "expected.h"
#include "winBase.h"

#include <string>

namespace pwr
{
	// A failed powrprof call: the Win32 code, the API that failed, and optional
	// context (which scheme/setting).
	struct PowerError {
		DWORD code = ERROR_SUCCESS;
		std::wstring operation; // e.g. "PowerReadACValueIndex"
		std::wstring context;   // e.g. "scheme=... setting=..."

		std::wstring systemMessage() const;

		// "PowerReadACValueIndex failed (5: Access is denied.) [context]"
		std::wstring describe() const;

		bool isAccessDenied() const {
			return code == ERROR_ACCESS_DENIED;
		}
		bool isPolicyDisabled() const {
			return code == ERROR_ACCESS_DISABLED_BY_POLICY;
		}
		bool isNotFound() const {
			return code == ERROR_FILE_NOT_FOUND || code == ERROR_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
		}
		bool isAlreadyExists() const {
			return code == ERROR_ALREADY_EXISTS || code == ERROR_OBJECT_ALREADY_EXISTS;
		}
	};

	std::wstring formatWin32Message(DWORD code);

	PowerError makeError(DWORD code, std::wstring operation, std::wstring context = {});

	inline Unexpected<PowerError> errResult(DWORD code, std::wstring operation, std::wstring context = {}) {
		return Unexpected<PowerError>{makeError(code, std::move(operation), std::move(context))};
	}

	template <typename T> using PowerResult = Expected<T, PowerError>;

}
