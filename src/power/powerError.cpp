#include "powerError.h"

namespace pwr
{
	std::wstring formatWin32Message(DWORD code) {
		LPWSTR raw = nullptr;
		const DWORD len = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		                                       FORMAT_MESSAGE_IGNORE_INSERTS,
		                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		                                   reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
		std::wstring text;
		if (len != 0 && raw != nullptr) {
			text.assign(raw, len);
			::LocalFree(raw);
			// FormatMessage appends CRLF; trim trailing whitespace.
			while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
				text.pop_back();
			}
		} else {
			text = L"(no system message)";
		}
		return text;
	}

	PowerError makeError(DWORD code, std::wstring operation, std::wstring context) {
		PowerError e;
		e.code = code;
		e.operation = std::move(operation);
		e.context = std::move(context);
		return e;
	}

	std::wstring PowerError::systemMessage() const {
		return formatWin32Message(code);
	}

	std::wstring PowerError::describe() const {
		std::wstring out = operation;
		out += L" failed (";
		out += std::to_wstring(code);
		out += L": ";
		out += systemMessage();
		out += L")";
		if (!context.empty()) {
			out += L" [";
			out += context;
			out += L"]";
		}
		return out;
	}

}
