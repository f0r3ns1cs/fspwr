#pragma once

// Ensures a directory exists; used when writing exported .pow files.

#include "powerError.h"

#include <filesystem>
namespace pwr
{
	PowerResult<std::filesystem::path> ensureDir(const std::filesystem::path& dir);
}
