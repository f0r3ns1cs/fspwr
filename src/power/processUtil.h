#pragma once

#include "powerError.h"

#include <string>

namespace pwr
{
	struct ProcessOutput {
		DWORD exitCode = 0;
		std::wstring output; // combined stdout+stderr, decoded
	};

	PowerResult<ProcessOutput> runProcessCaptured(const std::wstring& commandLine);
}
