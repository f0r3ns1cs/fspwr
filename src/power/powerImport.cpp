#include "powerImport.h"

#include "fsUtil.h"
#include "powerScheme.h"
#include "processUtil.h"

namespace pwr
{
	namespace fs = std::filesystem;

	PowerResult<void> exportPow(const GUID& scheme, const fs::path& outFile) {
		if (outFile.has_parent_path()) {
			auto dir = ensureDir(outFile.parent_path());
			if (!dir) return Unexpected<PowerError>{dir.error()};
		}

		const std::wstring cmd = L"powercfg /export \"" + outFile.wstring() + L"\" " + guidToString(scheme);
		auto result = runProcessCaptured(cmd);
		if (!result) return Unexpected<PowerError>{std::move(result).error()};
		if (result->exitCode != 0) {
			return errResult(result->exitCode ? result->exitCode : ERROR_INVALID_FUNCTION,
			                 L"powercfg /export", result->output);
		}

		std::error_code ec;
		if (!fs::exists(outFile, ec) || fs::file_size(outFile, ec) == 0 || ec) {
			return errResult(ERROR_INVALID_DATA, L"powercfg /export",
			                 L"reported success but " + outFile.wstring() + L" is missing or empty");
		}
		return {};
	}

	PowerResult<PowImporter::ImportOutcome> PowImporter::importPow(const fs::path& file,
	                                                               std::optional<GUID> destGuid) {
		// Validate the file up front so "file missing" doesn't surface as a
		// generic import failure.
		std::error_code ec;
		if (!fs::exists(file, ec)) {
			return errResult(ERROR_FILE_NOT_FOUND, L"importPow", file.wstring());
		}
		if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) == 0 || ec) {
			return errResult(ERROR_INVALID_DATA, L"importPow",
			                 file.wstring() + L" is empty or not a regular file");
		}

		const GUID dest = destGuid.value_or(generateGuid());
		SchemeManager schemes;
		if (schemes.schemeExists(dest)) {
			return errResult(ERROR_ALREADY_EXISTS, L"importPow",
			                 L"destination GUID " + guidToString(dest) + L" is already a power scheme");
		}

		GUID destCopy = dest;
		GUID* destPtr = &destCopy;
		const DWORD rc = ::PowerImportPowerScheme(nullptr, file.c_str(), &destPtr);
		if (rc != ERROR_SUCCESS || destPtr == nullptr) {
			return errResult(rc, L"PowerImportPowerScheme", file.wstring());
		}

		ImportOutcome outcome;
		outcome.scheme = *destPtr;
		return outcome;
	}

}
