#pragma once

#include "guidUtil.h"
#include "powerError.h"

#include <filesystem>
#include <optional>

namespace pwr
{
	class PowImporter {
	  public:
		struct ImportOutcome {
			GUID scheme{};
		};

		// destGuid fixes the imported plan's GUID; nullopt lets the OS pick one.
		// Distinct errors for a missing/unreadable file, a corrupt blob, and a GUID
		// collision.
		PowerResult<ImportOutcome> importPow(const std::filesystem::path& file, std::optional<GUID> destGuid);
	};

	// Runs powercfg /export, then checks the output file exists and is non-empty.
	PowerResult<void> exportPow(const GUID& scheme, const std::filesystem::path& outFile);

}
