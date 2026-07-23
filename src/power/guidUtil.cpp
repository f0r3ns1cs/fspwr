#include "guidUtil.h"

#include <rpc.h>

#include <cwctype>

namespace pwr
{
	std::wstring guidToString(const GUID& g) {
		wchar_t buf[37];
		::swprintf_s(buf, L"%08lx-%04hx-%04hx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx", g.Data1,
		             g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
		             g.Data4[6], g.Data4[7]);
		return buf;
	}

	namespace
	{
		std::optional<unsigned> hexVal(wchar_t c) {
			if (c >= L'0' && c <= L'9') return static_cast<unsigned>(c - L'0');
			if (c >= L'a' && c <= L'f') return static_cast<unsigned>(c - L'a' + 10);
			if (c >= L'A' && c <= L'F') return static_cast<unsigned>(c - L'A' + 10);
			return std::nullopt;
		}

		// Parses `count` hex digits from s at pos into out; advances pos.
		bool parseHex(std::wstring_view s, size_t& pos, int count, unsigned long long& out) {
			out = 0;
			for (int i = 0; i < count; ++i) {
				if (pos >= s.size()) return false;
				const auto v = hexVal(s[pos]);
				if (!v) return false;
				out = (out << 4) | *v;
				++pos;
			}
			return true;
		}

	}

	std::optional<GUID> guidFromString(std::wstring_view s) {
		// Trim whitespace and optional braces.
		while (!s.empty() && std::iswspace(s.front()))
			s.remove_prefix(1);
		while (!s.empty() && std::iswspace(s.back()))
			s.remove_suffix(1);
		if (s.size() >= 2 && s.front() == L'{' && s.back() == L'}') {
			s = s.substr(1, s.size() - 2);
		}
		if (s.size() != 36) return std::nullopt;

		size_t pos = 0;
		unsigned long long d1 = 0, d2 = 0, d3 = 0;
		GUID g{};
		if (!parseHex(s, pos, 8, d1)) return std::nullopt;
		if (pos >= s.size() || s[pos++] != L'-') return std::nullopt;
		if (!parseHex(s, pos, 4, d2)) return std::nullopt;
		if (pos >= s.size() || s[pos++] != L'-') return std::nullopt;
		if (!parseHex(s, pos, 4, d3)) return std::nullopt;
		if (pos >= s.size() || s[pos++] != L'-') return std::nullopt;
		g.Data1 = static_cast<unsigned long>(d1);
		g.Data2 = static_cast<unsigned short>(d2);
		g.Data3 = static_cast<unsigned short>(d3);
		for (int i = 0; i < 8; ++i) {
			if (i == 2) {
				if (pos >= s.size() || s[pos++] != L'-') return std::nullopt;
			}
			unsigned long long b = 0;
			if (!parseHex(s, pos, 2, b)) return std::nullopt;
			g.Data4[i] = static_cast<unsigned char>(b);
		}
		if (pos != s.size()) return std::nullopt;
		return g;
	}

	GUID generateGuid() {
		GUID g{};
		// RPC_S_UUID_LOCAL_ONLY still produces a unique GUID, which is all we need.
		(void)::UuidCreate(&g);
		return g;
	}

	bool guidEqual(const GUID& a, const GUID& b) {
		return ::memcmp(&a, &b, sizeof(GUID)) == 0;
	}

	bool GuidLess::operator()(const GUID& a, const GUID& b) const noexcept {
		return ::memcmp(&a, &b, sizeof(GUID)) < 0;
	}

	size_t GuidHash::operator()(const GUID& g) const noexcept {
		// FNV-1a over the 16 raw bytes.
		const unsigned char* p = reinterpret_cast<const unsigned char*>(&g);
		size_t h = 14695981039346656037ull;
		for (size_t i = 0; i < sizeof(GUID); ++i) {
			h ^= p[i];
			h *= 1099511628211ull;
		}
		return h;
	}

	namespace known
	{
		namespace
		{
			GUID mustParse(const wchar_t* s) {
				return *guidFromString(s);
			}
		}

		const GUID SchemeBalanced = mustParse(L"381b4222-f694-41f0-9685-ff5bb260df2e");
		const GUID SchemeHighPerformance = mustParse(L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c");
		const GUID SchemePowerSaver = mustParse(L"a1841308-3541-4fab-bc81-f71556f20b4a");
		const GUID SchemeUltimate = mustParse(L"e9a42b02-d5df-448d-aa00-03f14749eb61");

		const GUID NoSubgroup = mustParse(L"fea3413e-7e05-4911-9a71-700331f1c294");
		const GUID SettingPersonality = mustParse(L"245d8541-3943-4422-b025-13a784f679b7");

	}

	bool isBuiltinScheme(const GUID& g) {
		return guidEqual(g, known::SchemeBalanced) || guidEqual(g, known::SchemeHighPerformance) ||
		       guidEqual(g, known::SchemePowerSaver) || guidEqual(g, known::SchemeUltimate);
	}

}
