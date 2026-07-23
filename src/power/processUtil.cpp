#include "processUtil.h"

#include <vector>

namespace pwr
{
	namespace
	{
		struct HandleHolder {
			HANDLE h = nullptr;
			~HandleHolder() {
				if (h) ::CloseHandle(h);
			}
		};

		// powercfg writes OEM/console codepage text when redirected. Decode with the
		// console output codepage, falling back to ACP.
		std::wstring decodeConsoleBytes(const std::string& bytes) {
			if (bytes.empty()) return {};
			// UTF-16LE BOM check (some tools emit wide text when redirected).
			if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
			    static_cast<unsigned char>(bytes[1]) == 0xFE) {
				return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
				                    (bytes.size() - 2) / sizeof(wchar_t));
			}
			UINT cp = ::GetConsoleOutputCP();
			if (cp == 0) cp = ::GetOEMCP();
			int needed =
			    ::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
			if (needed <= 0) {
				cp = CP_ACP;
				needed =
				    ::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
				if (needed <= 0) return {};
			}
			std::wstring out(static_cast<size_t>(needed), L'\0');
			::MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), needed);
			return out;
		}

	}

	PowerResult<ProcessOutput> runProcessCaptured(const std::wstring& commandLine) {
		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;

		HandleHolder readPipe, writePipe;
		if (!::CreatePipe(&readPipe.h, &writePipe.h, &sa, 0)) {
			return errResult(::GetLastError(), L"CreatePipe");
		}
		if (!::SetHandleInformation(readPipe.h, HANDLE_FLAG_INHERIT, 0)) {
			return errResult(::GetLastError(), L"SetHandleInformation");
		}

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = writePipe.h;
		si.hStdError = writePipe.h;
		si.hStdInput = nullptr;

		PROCESS_INFORMATION pi{};
		std::wstring mutableCmd = commandLine; // CreateProcessW may modify the buffer
		if (!::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
		                      nullptr, &si, &pi)) {
			return errResult(::GetLastError(), L"CreateProcess", commandLine);
		}
		HandleHolder process{pi.hProcess}, thread{pi.hThread};

		// Close our copy of the write end so ReadFile sees EOF when the child exits.
		::CloseHandle(writePipe.h);
		writePipe.h = nullptr;

		std::string bytes;
		char buf[4096];
		DWORD read = 0;
		while (::ReadFile(readPipe.h, buf, sizeof(buf), &read, nullptr) && read > 0) {
			bytes.append(buf, read);
		}

		::WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD exitCode = 0;
		if (!::GetExitCodeProcess(pi.hProcess, &exitCode)) {
			return errResult(::GetLastError(), L"GetExitCodeProcess", commandLine);
		}

		ProcessOutput out;
		out.exitCode = exitCode;
		out.output = decodeConsoleBytes(bytes);
		return out;
	}

}
