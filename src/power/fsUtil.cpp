#include "fsUtil.h"

namespace pwr
{
	namespace fs = std::filesystem;

	PowerResult<fs::path> ensureDir(const fs::path& dir) {
		std::error_code ec;
		fs::create_directories(dir, ec);
		if (ec) {
			return errResult(static_cast<DWORD>(ec.value()), L"create_directories", dir.wstring());
		}
		return dir;
	}

}
