#include "powerSettings.h"

#include "accessCheck.h"
#include "powerBuffers.h"

#ifndef POWER_ATTRIBUTE_HIDE
#define POWER_ATTRIBUTE_HIDE 0x00000001
#endif

namespace pwr
{
	namespace
	{
		std::wstring ctx(const GUID& scheme, const GUID& subgroup, const GUID& setting) {
			return L"scheme=" + guidToString(scheme) + L" subgroup=" + guidToString(subgroup) + L" setting=" +
			       guidToString(setting);
		}

		PowerResult<std::vector<GUID>> enumerateChildren(const GUID& scheme, const GUID* subgroup,
		                                                 POWER_DATA_ACCESSOR accessor, const wchar_t* op) {
			std::vector<GUID> out;
			for (ULONG i = 0;; ++i) {
				GUID g{};
				DWORD size = sizeof(g);
				const DWORD rc = ::PowerEnumerate(nullptr, &scheme, subgroup, accessor, i,
				                                  reinterpret_cast<UCHAR*>(&g), &size);
				if (rc == ERROR_NO_MORE_ITEMS) break;
				if (rc != ERROR_SUCCESS) {
					return errResult(rc, op,
					                 L"scheme=" + guidToString(scheme) + L" index=" + std::to_wstring(i));
				}
				out.push_back(g);
			}
			return out;
		}

		std::wstring readNameOrEmpty(const GUID& scheme, const GUID* subgroup, const GUID* setting,
		                             std::vector<PowerError>& warnings) {
			auto r = readPowerString(L"PowerReadFriendlyName", L"", [&](UCHAR* buf, DWORD* size) {
				return ::PowerReadFriendlyName(nullptr, &scheme, subgroup, setting, buf, size);
			});
			if (!r) {
				// Missing names are common for OEM/hidden settings; degrade quietly.
				if (r.error().code != ERROR_FILE_NOT_FOUND) warnings.push_back(r.error());
				return {};
			}
			return std::move(*r);
		}

		std::wstring readDescriptionOrEmpty(const GUID& scheme, const GUID* subgroup, const GUID* setting) {
			auto r = readPowerString(L"PowerReadDescription", L"", [&](UCHAR* buf, DWORD* size) {
				return ::PowerReadDescription(nullptr, &scheme, subgroup, setting, buf, size);
			});
			return r ? std::move(*r) : std::wstring{};
		}

		void detectKind(const GUID& scheme, const GUID& subgroup, SettingDescriptor& d,
		                std::vector<PowerError>& warnings) {
			DWORD minV = 0, maxV = 0, inc = 0;
			const DWORD rcMin = ::PowerReadValueMin(nullptr, &subgroup, &d.id, &minV);
			const DWORD rcMax = ::PowerReadValueMax(nullptr, &subgroup, &d.id, &maxV);
			if (rcMin == ERROR_SUCCESS && rcMax == ERROR_SUCCESS) {
				d.kind = SettingKind::Range;
				d.min = minV;
				d.max = maxV;
				d.increment =
				    (::PowerReadValueIncrement(nullptr, &subgroup, &d.id, &inc) == ERROR_SUCCESS && inc > 0)
				        ? inc
				        : 1;
				auto units =
				    readPowerString(L"PowerReadValueUnitsSpecifier", L"", [&](UCHAR* buf, DWORD* size) {
					    return ::PowerReadValueUnitsSpecifier(nullptr, &subgroup, &d.id, buf, size);
				    });
				if (units) d.units = std::move(*units);
				return;
			}

			// Not a range: try the possible-value list.
			for (ULONG i = 0;; ++i) {
				ULONG type = 0;
				auto valueBuf = readPowerBuffer(L"PowerReadPossibleValue", L"", [&](UCHAR* buf, DWORD* size) {
					return ::PowerReadPossibleValue(nullptr, &subgroup, &d.id, &type, i, buf, size);
				});
				if (!valueBuf) {
					if (valueBuf.error().code != ERROR_NO_MORE_ITEMS && i == 0) {
						// Neither range nor enum metadata: Opaque, raw index editing.
						d.kind = SettingKind::Opaque;
						d.metadataIncomplete = true;
						return;
					}
					if (valueBuf.error().code != ERROR_NO_MORE_ITEMS) {
						warnings.push_back(valueBuf.error());
					}
					break;
				}

				PossibleValue pv;
				pv.index = i;
				// The possible-value payload is normally a DWORD index; when present
				// and sane, prefer it over the ordinal.
				if (valueBuf->size() >= sizeof(DWORD)) {
					pv.index = *reinterpret_cast<const DWORD*>(valueBuf->data());
				}
				auto name =
				    readPowerString(L"PowerReadPossibleFriendlyName", L"", [&](UCHAR* buf, DWORD* size) {
					    return ::PowerReadPossibleFriendlyName(nullptr, &subgroup, &d.id, i, buf, size);
				    });
				if (name) pv.name = std::move(*name);
				auto desc =
				    readPowerString(L"PowerReadPossibleDescription", L"", [&](UCHAR* buf, DWORD* size) {
					    return ::PowerReadPossibleDescription(nullptr, &subgroup, &d.id, i, buf, size);
				    });
				if (desc) pv.description = std::move(*desc);
				d.possibleValues.push_back(std::move(pv));
			}

			if (!d.possibleValues.empty()) {
				d.kind = SettingKind::Enum;
			} else {
				d.kind = SettingKind::Opaque;
				d.metadataIncomplete = true;
				(void)scheme;
			}
		}

		std::optional<DWORD> readIndexOpt(DWORD(WINAPI* fn)(HKEY, const GUID*, const GUID*, const GUID*,
		                                                    LPDWORD),
		                                  const wchar_t* op, const GUID& scheme, const GUID& subgroup,
		                                  const GUID& setting, std::vector<PowerError>& warnings) {
			DWORD v = 0;
			const DWORD rc = fn(nullptr, &scheme, &subgroup, &setting, &v);
			if (rc == ERROR_SUCCESS) return v;
			// Absent AC or DC values are legitimate (e.g. desktop machines often lack
			// DC data); only unexpected codes are warnings.
			if (rc != ERROR_FILE_NOT_FOUND && rc != ERROR_NOT_FOUND) {
				warnings.push_back(makeError(rc, op, ctx(scheme, subgroup, setting)));
			}
			return std::nullopt;
		}

	}

	std::wstring SettingDescriptor::renderValue(std::optional<DWORD> v) const {
		if (!v) return L"-";
		if (kind == SettingKind::Enum) {
			for (const auto& pv : possibleValues) {
				if (pv.index == *v && !pv.name.empty()) {
					return pv.name;
				}
			}
		}
		std::wstring out = std::to_wstring(*v);
		if (!units.empty()) {
			out += L" ";
			out += units;
		}
		return out;
	}

	GUID SettingsEnumerator::resolveDefaultsScheme(const GUID& scheme, std::optional<GUID> knownBase) {
		DWORD idx = 0;
		const DWORD rc =
		    ::PowerReadACValueIndex(nullptr, &scheme, &known::NoSubgroup, &known::SettingPersonality, &idx);
		if (rc == ERROR_SUCCESS) {
			switch (idx) {
			case 0: return known::SchemePowerSaver;
			case 1: return known::SchemeBalanced;
			case 2: return known::SchemeHighPerformance;
			default: break;
			}
		}
		if (knownBase) return *knownBase;
		return known::SchemeBalanced;
	}

	PowerResult<SchemeEnumeration> SettingsEnumerator::enumerateScheme(const GUID& scheme,
	                                                                   std::optional<GUID> knownBase) {
		SchemeEnumeration out;
		out.scheme = scheme;
		out.defaultsScheme = resolveDefaultsScheme(scheme, knownBase);

		auto subgroups =
		    enumerateChildren(scheme, nullptr, ACCESS_SUBGROUP, L"PowerEnumerate(ACCESS_SUBGROUP)");
		if (!subgroups) return Unexpected<PowerError>{std::move(subgroups).error()};

		// NO_SUBGROUP holds settings that sit directly under the scheme; do it first.
		std::vector<GUID> ordered;
		ordered.push_back(known::NoSubgroup);
		for (const GUID& g : *subgroups) {
			if (!guidEqual(g, known::NoSubgroup)) ordered.push_back(g);
		}

		PolicyChecker policy;

		for (const GUID& subgroup : ordered) {
			SubgroupInfo sg;
			sg.id = subgroup;
			sg.name = readNameOrEmpty(scheme, &subgroup, nullptr, out.warnings);
			if (sg.name.empty()) {
				sg.name = guidEqual(subgroup, known::NoSubgroup) ? L"Settings without a subgroup"
				                                                 : guidToString(subgroup);
			}
			sg.description = readDescriptionOrEmpty(scheme, &subgroup, nullptr);
			out.subgroups.push_back(sg);

			auto settings = enumerateChildren(scheme, &subgroup, ACCESS_INDIVIDUAL_SETTING,
			                                  L"PowerEnumerate(ACCESS_INDIVIDUAL_SETTING)");
			if (!settings) {
				out.warnings.push_back(settings.error());
				continue;
			}

			for (const GUID& setting : *settings) {
				SettingEntry entry;
				SettingDescriptor& d = entry.desc;
				d.subgroup = subgroup;
				d.id = setting;
				d.name = readNameOrEmpty(scheme, &subgroup, &setting, out.warnings);
				if (d.name.empty()) {
					d.name = guidToString(setting);
					d.metadataIncomplete = true;
				}
				d.description = readDescriptionOrEmpty(scheme, &subgroup, &setting);

				const DWORD attrs = ::PowerReadSettingAttributes(&subgroup, &setting);
				d.hidden = (attrs & POWER_ATTRIBUTE_HIDE) != 0;

				detectKind(scheme, subgroup, d, out.warnings);

				d.acLockedByPolicy =
				    policy.settingWriteState(setting, PowerRail::Ac) == PolicyState::DisabledByPolicy;
				d.dcLockedByPolicy =
				    policy.settingWriteState(setting, PowerRail::Dc) == PolicyState::DisabledByPolicy;

				entry.values.ac = readIndexOpt(&PowerReadACValueIndex, L"PowerReadACValueIndex", scheme,
				                               subgroup, setting, out.warnings);
				entry.values.dc = readIndexOpt(&PowerReadDCValueIndex, L"PowerReadDCValueIndex", scheme,
				                               subgroup, setting, out.warnings);
				entry.values.acDefault = readIndexOpt(&PowerReadACDefaultIndex, L"PowerReadACDefaultIndex",
				                                      out.defaultsScheme, subgroup, setting, out.warnings);
				entry.values.dcDefault = readIndexOpt(&PowerReadDCDefaultIndex, L"PowerReadDCDefaultIndex",
				                                      out.defaultsScheme, subgroup, setting, out.warnings);

				out.settings.push_back(std::move(entry));
			}
		}

		return out;
	}

}
