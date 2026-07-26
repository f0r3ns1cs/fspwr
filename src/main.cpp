#include "power/guidUtil.h"
#include "power/platform.h"
#include "power/powerImport.h"
#include "power/powerScheme.h"
#include "power/powerSettings.h"
#include "power/powerValues.h"
#include "power/worker.h"

#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace pwr;

namespace
{
	constexpr wchar_t kClassName[] = L"fspwrWindow";
	constexpr wchar_t kDetailClassName[] = L"fspwrDetailPanel";
	constexpr UINT WM_APP_ENUM_DONE = WM_APP + 1;
	constexpr UINT WM_APP_CMP_DONE = WM_APP + 2;
	constexpr UINT WM_APP_PLAN_SWITCH = WM_APP + 3;

	// Command and control IDs. Menu items and their matching context-menu entries
	// share these, so both route to the same WM_COMMAND handlers.
	enum : int {
		ID_SCHEMES = 2001,
		ID_CREATE,
		ID_IMPORT,
		ID_DIFF,
		ID_TITLE,
		ID_REFRESH,
		ID_ACTIVATE,
		ID_RENAME,
		ID_DESC,
		ID_DUP,
		ID_EXPORT,
		ID_DELETE,
		ID_ADVISORY,
		ID_SUBGROUP,
		ID_SEARCH,
		ID_HIDDEN,
		ID_MODIFIED,
		ID_SETTINGS,
		ID_PENDLABEL,
		ID_VIEWDIFF,
		ID_DISCARD,
		ID_APPLY,
		ID_STATUS,
		ID_SCHEMES_LABEL,
		ID_ABOUT,
		ID_EXIT,
		// Compare mode: the source-plan picker and the copy actions that pull
		// values from it into the plan currently being edited.
		ID_CMPLABEL,
		ID_COMPARE,
		ID_DIFFONLY,
		ID_COPYSEL,
		ID_COPYALL,
		// Settings-list context menu.
		ID_CTX_COPY,
		ID_CTX_COPY_AC,
		ID_CTX_COPY_DC,
		ID_CTX_REVERT,
	};

	enum class LoadState { Loading, Ready, Error };

	// Per-setting relationship between the edited plan and the compare source.
	enum class DiffState {
		Staged,    // a pending change already covers this setting
		Same,      // both plans agree on both rails
		Differs,   // present in both, values disagree
		OnlyHere,  // the source plan does not have this setting at all
		OnlyThere, // no value on either rail here, but the source has one
	};

	// Deliberately non-directional: the copy direction is fixed (source into the
	// edited plan), so arrows here would only invite the wrong reading.
	const wchar_t* diffGlyph(DiffState s) {
		switch (s) {
		case DiffState::Staged: return L"✓";
		case DiffState::Same: return L"=";
		case DiffState::Differs: return L"≠";
		case DiffState::OnlyHere: return L"·";
		case DiffState::OnlyThere: return L"+";
		}
		return L"";
	}

	// Identifies a setting across two enumerations.
	struct SettingKey {
		GUID sub{}, id{};
	};

	struct SettingKeyLess {
		bool operator()(const SettingKey& a, const SettingKey& b) const noexcept {
			const GuidLess less;
			if (less(a.sub, b.sub)) return true;
			if (less(b.sub, a.sub)) return false;
			return less(a.id, b.id);
		}
	};

	bool containsNoCase(const std::wstring& hay, const std::wstring& needle) {
		if (needle.empty()) return true;
		auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
		                      [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
		return it != hay.end();
	}

	std::wstring indexText(std::optional<DWORD> v) {
		if (!v) return L"-";
		return std::to_wstring(*v);
	}

	// Enumeration result posted from the worker thread to the UI thread.
	struct EnumResult {
		unsigned generation = 0;
		bool ok = false;
		SchemeEnumeration enumeration;
		std::wstring error;
	};

	// diff between two enumerated plans
	struct DiffRow {
		std::wstring name, subgroup, acA, acB, dcA, dcB;
	};

	std::vector<DiffRow> computeDiff(const SchemeEnumeration& A, const SchemeEnumeration& B) {
		std::map<GUID, std::wstring, GuidLess> subNames;
		for (const auto& sg : A.subgroups)
			subNames[sg.id] = sg.name;
		for (const auto& sg : B.subgroups)
			subNames.emplace(sg.id, sg.name);
		auto subName = [&](const GUID& id) -> std::wstring {
			auto it = subNames.find(id);
			return it != subNames.end() ? it->second : guidToString(id);
		};
		auto findIn = [](const SchemeEnumeration& e, const GUID& sub,
		                 const GUID& set) -> const SettingEntry* {
			for (const auto& s : e.settings)
				if (guidEqual(s.desc.subgroup, sub) && guidEqual(s.desc.id, set)) return &s;
			return nullptr;
		};

		std::vector<DiffRow> rows;
		for (const auto& a : A.settings) {
			const SettingEntry* b = findIn(B, a.desc.subgroup, a.desc.id);
			if (b && a.values.ac == b->values.ac && a.values.dc == b->values.dc) continue;
			DiffRow r;
			r.name = a.desc.name;
			r.subgroup = subName(a.desc.subgroup);
			r.acA = a.desc.renderValue(a.values.ac);
			r.dcA = a.desc.renderValue(a.values.dc);
			r.acB = b ? b->desc.renderValue(b->values.ac) : L"(absent)";
			r.dcB = b ? b->desc.renderValue(b->values.dc) : L"(absent)";
			rows.push_back(std::move(r));
		}
		// Settings only present in B.
		for (const auto& b : B.settings) {
			if (findIn(A, b.desc.subgroup, b.desc.id)) continue;
			DiffRow r;
			r.name = b.desc.name;
			r.subgroup = subName(b.desc.subgroup);
			r.acA = L"(absent)";
			r.dcA = L"(absent)";
			r.acB = b.desc.renderValue(b.values.ac);
			r.dcB = b.desc.renderValue(b.values.dc);
			rows.push_back(std::move(r));
		}
		return rows;
	}

	// file dialogs
	std::optional<std::wstring> openFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title,
	                                           const std::wstring& dir) {
		wchar_t buf[MAX_PATH]{};
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = owner;
		ofn.lpstrFilter = filter;
		ofn.lpstrFile = buf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = title;
		ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
		return ::GetOpenFileNameW(&ofn) ? std::optional<std::wstring>(buf) : std::nullopt;
	}

	std::optional<std::wstring> saveFileDialog(HWND owner, const wchar_t* filter, const wchar_t* defExt,
	                                           std::wstring defName, const wchar_t* title) {
		wchar_t buf[MAX_PATH]{};
		::wcsncpy_s(buf, defName.c_str(), _TRUNCATE);
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = owner;
		ofn.lpstrFilter = filter;
		ofn.lpstrFile = buf;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrTitle = title;
		ofn.lpstrDefExt = defExt;
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOREADONLYRETURN;
		return ::GetSaveFileNameW(&ofn) ? std::optional<std::wstring>(buf) : std::nullopt;
	}

	// resource dialog data
	struct CreateDlgData {
		std::vector<std::wstring> baseNames;
		size_t baseSel = 0;
		std::wstring name, desc, guid;
		bool ok = false;
	};

	struct PromptDlgData {
		std::wstring title, label, value;
		bool ok = false;
	};

	INT_PTR CALLBACK createDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
		auto* d = reinterpret_cast<CreateDlgData*>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
		switch (msg) {
		case WM_INITDIALOG: {
			d = reinterpret_cast<CreateDlgData*>(lParam);
			::SetWindowLongPtrW(dlg, GWLP_USERDATA, lParam);
			for (const auto& n : d->baseNames) {
				::SendDlgItemMessageW(dlg, IDC_BASE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
			}
			::SendDlgItemMessageW(dlg, IDC_BASE, CB_SETCURSEL, d->baseSel, 0);
			::SetDlgItemTextW(dlg, IDC_GUID, d->guid.c_str());
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(wParam)) {
			case IDC_REGEN:
				::SetDlgItemTextW(dlg, IDC_GUID, guidToString(generateGuid()).c_str());
				return TRUE;
			case IDOK: {
				wchar_t buf[512];
				::GetDlgItemTextW(dlg, IDC_NAME, buf, 512);
				d->name = buf;
				::GetDlgItemTextW(dlg, IDC_DESC, buf, 512);
				d->desc = buf;
				::GetDlgItemTextW(dlg, IDC_GUID, buf, 512);
				d->guid = buf;
				d->baseSel = static_cast<size_t>(::SendDlgItemMessageW(dlg, IDC_BASE, CB_GETCURSEL, 0, 0));
				if (d->name.empty()) {
					::MessageBoxW(dlg, L"A plan name is required.", L"Create plan", MB_ICONWARNING);
					return TRUE;
				}
				if (!guidFromString(d->guid)) {
					::MessageBoxW(dlg, L"The GUID is not in a valid format.", L"Create plan", MB_ICONWARNING);
					return TRUE;
				}
				d->ok = true;
				::EndDialog(dlg, IDOK);
				return TRUE;
			}
			case IDCANCEL: ::EndDialog(dlg, IDCANCEL); return TRUE;
			}
			break;
		}
		return FALSE;
	}

	INT_PTR CALLBACK promptDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
		auto* d = reinterpret_cast<PromptDlgData*>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
		switch (msg) {
		case WM_INITDIALOG:
			d = reinterpret_cast<PromptDlgData*>(lParam);
			::SetWindowLongPtrW(dlg, GWLP_USERDATA, lParam);
			::SetWindowTextW(dlg, d->title.c_str());
			::SetDlgItemTextW(dlg, IDC_PROMPT_LABEL, d->label.c_str());
			::SetDlgItemTextW(dlg, IDC_PROMPT_EDIT, d->value.c_str());
			::SendDlgItemMessageW(dlg, IDC_PROMPT_EDIT, EM_SETSEL, 0, -1);
			return TRUE;
		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK) {
				wchar_t buf[512];
				::GetDlgItemTextW(dlg, IDC_PROMPT_EDIT, buf, 512);
				d->value = buf;
				d->ok = true;
				::EndDialog(dlg, IDOK);
				return TRUE;
			}
			if (LOWORD(wParam) == IDCANCEL) {
				::EndDialog(dlg, IDCANCEL);
				return TRUE;
			}
			break;
		}
		return FALSE;
	}

}

class App {
  public:
	int run(HINSTANCE instance, int showCmd);

  private:
	// window / layout
	static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
	LRESULT wndProc(UINT, WPARAM, LPARAM);
	void createControls();
	void layout();
	int S(int dip) const {
		return ::MulDiv(dip, static_cast<int>(dpi_), 96);
	}
	HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0);
	void applyFonts();

	// data / worker
	void loadSchemes(std::optional<GUID> preferred = std::nullopt);
	void refreshSchemeList();
	void startEnumeration();
	void onEnumDone(EnumResult* res);
	void rebuildSubgroupCombo(std::optional<GUID> keep = std::nullopt);
	void rebuildFilter();
	void rebuildDetail();
	void clearDetail();
	const SettingEntry* rowEntry(size_t row) const;
	const SettingEntry* findEntry(const GUID& sub, const GUID& id) const;
	std::wstring subgroupName(const GUID& id) const;
	bool isModified(const SettingEntry& e) const;
	std::optional<DWORD> effectiveValue(const SettingEntry& e, bool ac) const;
	const GUID* selectedSchemeGuid() const;
	int selectedSettingRow() const;
	std::vector<size_t> selectedSettingIndices() const;
	void updateStatus();
	void updatePendingBar();
	bool confirmDiscardPending(const wchar_t* action);

	// compare mode
	void rebuildCompareCombo();
	void setCompareSource(std::optional<GUID> source);
	void startCompareEnumeration();
	void onCompareDone(EnumResult* res);
	void rebuildSettingColumns();
	const SettingEntry* sourceEntry(const GUID& sub, const GUID& id) const;
	DiffState diffStateOf(const SettingEntry& e) const;
	size_t differenceCount() const;
	void recomputeSourceOnly();

	// copying values across from the compare source
	struct CopySummary {
		size_t staged = 0;    // values staged for Apply
		size_t identical = 0; // already equal: the normal majority, never reported
		size_t noValue = 0;   // source has no value on that rail; visible as "-" in the grid
		// Only genuinely blocked values land here, named, so the report can be
		// checked against the grid instead of being taken on faith.
		std::vector<std::wstring> blocked;
	};
	void stageFromSource(const SettingEntry& target, bool ac, CopySummary& sum);
	void copyFromSource(const std::vector<size_t>& indices, bool doAc, bool doDc, const wchar_t* scope);

	// detail editors
	struct Editor {
		// Reset writes the plan's own default; Adopt writes the compare source's
		// value. Both are one-click stages of a value known up front.
		enum Type { Combo, Trackbar, Spinner, Reset, Adopt, GuidCopy } type;
		GUID sub{}, setting{};
		bool ac = false;
		std::vector<DWORD> enumValues;
		DWORD actionValue = 0; // Reset: the default; Adopt: the source's value
		std::wstring guidText;
		HWND header = nullptr; // trackbar: static to update with live value
		std::wstring headerBase;
	};
	HWND addDetailLine(const wchar_t* cls, const wchar_t* text, DWORD style, int height, DWORD exStyle = 0);
	void addRailEditor(const SettingEntry& e, bool ac);
	void addCompareRail(const SettingEntry& e, bool ac, bool locked, std::optional<DWORD> current);
	void onEditorCommand(HWND ctrl, int code);
	void onTrackbar(HWND ctrl);
	void layoutDetail(); // stacks the detail children and updates its scrollbar
	void onDetailScroll(int action, int thumbPos);
	void onDetailWheel(int delta);

	// actions
	// validate guards hand-entered input. Values that came from Windows itself
	// (a plan's own default, or another plan's live value) are attested and must
	// not be re-judged against advisory range/increment metadata.
	void stageValue(const GUID& sub, const GUID& id, bool ac, DWORD value, bool validate = true);
	void applyPending();
	void discardPending();
	void showPendingDiff();
	void showCreator();
	void activateSelected();
	void renameSelected();
	void editDescriptionSelected();
	void duplicateSelected();
	void deleteSelected();
	void browseImport();
	void importPow(const std::wstring& path);
	void exportSelected();
	void showDiff();
	void runDiff(HWND dlg);
	static INT_PTR CALLBACK diffDlgProc(HWND, UINT, WPARAM, LPARAM);
	void showAbout();

	// menus
	void buildMenu();
	void updateMenuState();
	void showSchemeMenu(int screenX, int screenY);
	void showSettingMenu(int screenX, int screenY);

	// helpers
	void showError(const std::wstring& title, const PowerError& e);
	void showInfo(const std::wstring& title, const std::wstring& msg);
	void copyToClipboard(const std::wstring& text);

	HINSTANCE instance_ = nullptr;
	HWND hwnd_ = nullptr;
	HWND detailPanel_ = nullptr;
	HACCEL accel_ = nullptr;
	UINT dpi_ = 96;
	HFONT font_ = nullptr;
	HFONT fontTitle_ = nullptr;

	SchemeManager schemes_;
	SettingsEnumerator enumerator_;
	ValueAccessor values_;

	std::vector<SchemeInfo> schemeList_;
	int selectedScheme_ = -1;
	SchemeEnumeration enumeration_;
	LoadState enumState_ = LoadState::Loading;
	std::wstring enumError_;
	PendingChangeSet pending_;
	bool applying_ = false;
	unsigned enumGeneration_ = 0;

	bool suppressSchemeSelect_ = false; // guards LVN_ITEMCHANGED during list rebuild
	std::vector<size_t> filtered_;
	int selectedSubgroup_ = 0;
	std::wstring filterText_;
	bool filterHidden_ = false;
	bool filterModified_ = false;
	bool filterDiffOnly_ = false;

	// Compare source: a second plan enumerated alongside the edited one, whose
	// values can be pulled across. Held by GUID so it survives list reloads.
	bool compareOn_ = false;
	GUID compareSource_{};
	std::wstring compareName_;
	SchemeEnumeration compareEnum_;
	std::map<SettingKey, size_t, SettingKeyLess> compareIndex_;
	LoadState compareState_ = LoadState::Loading;
	unsigned compareGeneration_ = 0;
	size_t sourceOnly_ = 0; // settings the source has that this plan does not enumerate
	std::vector<size_t> compareCombo_; // combo item (1-based) -> schemeList_ index
	bool compareColumns_ = false;      // which column set the settings list has

	std::vector<HWND> detailControls_;
	std::vector<int> detailHeights_;
	int detailScroll_ = 0;
	std::optional<SettingKey> detailKey_; // setting the pane currently describes
	std::map<HWND, Editor> editors_;
	std::vector<DiffRow> diffRows_;
	int diffMergeA_ = -1, diffMergeB_ = -1; // set by the compare dialog's Merge button
};

// window class + entry

LRESULT CALLBACK App::wndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	App* self = nullptr;
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
		self = static_cast<App*>(cs->lpCreateParams);
		self->hwnd_ = hwnd;
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	} else {
		self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}
	if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
	return self->wndProc(msg, wParam, lParam);
}

int App::run(HINSTANCE instance, int showCmd) {
	instance_ = instance;

	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS |
	                                          ICC_STANDARD_CLASSES};
	::InitCommonControlsEx(&icc);

	// Detail panel child window class: hosts the dynamic editors and forwards
	// their notifications to the main window (they are its children, not the
	// main window's). The dialog-face background matches the main window.
	// It scrolls itself: the stacked editors can exceed the panel height once a
	// long description and the compare-source rows are in play.
	WNDCLASSW dpc{};
	dpc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
		if (m == WM_COMMAND || m == WM_HSCROLL || m == WM_NOTIFY) {
			return ::SendMessageW(::GetParent(h), m, w, l);
		}
		if (m == WM_VSCROLL || m == WM_MOUSEWHEEL) {
			if (auto* self = reinterpret_cast<App*>(::GetWindowLongPtrW(h, GWLP_USERDATA))) {
				if (m == WM_VSCROLL)
					self->onDetailScroll(LOWORD(w), HIWORD(w));
				else
					self->onDetailWheel(GET_WHEEL_DELTA_WPARAM(w));
				return 0;
			}
		}
		return ::DefWindowProcW(h, m, w, l);
	};
	dpc.hInstance = instance;
	dpc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	dpc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	dpc.lpszClassName = kDetailClassName;
	::RegisterClassW(&dpc);

	WNDCLASSW wc{};
	wc.lpfnWndProc = &App::wndProcThunk;
	wc.hInstance = instance;
	wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
	wc.lpszClassName = kClassName;
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	::RegisterClassW(&wc);

	hwnd_ = ::CreateWindowExW(WS_EX_ACCEPTFILES, kClassName, L"fspwr", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
	                          CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, this);
	if (!hwnd_) {
		::MessageBoxW(nullptr, L"Failed to create the application window.", L"fspwr", MB_ICONERROR);
		return 1;
	}

	dpi_ = ::GetDpiForWindow(hwnd_);
	applyFonts();
	buildMenu();
	createControls();
	::DragAcceptFiles(hwnd_, TRUE);

	// Alt+Left mirrors WinMerge: the arrow points at the destination, which here
	// is always the plan being edited (the left-hand columns).
	const ACCEL accels[] = {
	    {FVIRTKEY | FCONTROL, 'O', ID_IMPORT}, {FVIRTKEY | FCONTROL, 'E', ID_EXPORT},
	    {FVIRTKEY | FCONTROL, 'N', ID_CREATE}, {FVIRTKEY | FCONTROL, 'S', ID_APPLY},
	    {FVIRTKEY, VK_F2, ID_RENAME},          {FVIRTKEY, VK_F5, ID_REFRESH},
	    {FVIRTKEY | FALT, VK_LEFT, ID_COPYSEL},
	};
	accel_ = ::CreateAcceleratorTableW(const_cast<ACCEL*>(accels), static_cast<int>(std::size(accels)));

	::ShowWindow(hwnd_, showCmd);
	::UpdateWindow(hwnd_);

	loadSchemes();

	MSG msg{};
	while (::GetMessageW(&msg, nullptr, 0, 0)) {
		if (accel_ && ::TranslateAcceleratorW(hwnd_, accel_, &msg)) continue;
		if (::IsDialogMessageW(hwnd_, &msg)) continue; // Tab / arrow navigation
		::TranslateMessage(&msg);
		::DispatchMessageW(&msg);
	}
	return static_cast<int>(msg.wParam);
}

// fonts

void App::applyFonts() {
	if (font_) ::DeleteObject(font_);
	if (fontTitle_) ::DeleteObject(fontTitle_);
	NONCLIENTMETRICSW ncm{sizeof(ncm)};
	::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
	font_ = ::CreateFontIndirectW(&ncm.lfMessageFont);
	LOGFONTW title = ncm.lfMessageFont;
	title.lfHeight = ::MulDiv(title.lfHeight, 14, 10);
	title.lfWeight = FW_SEMIBOLD;
	fontTitle_ = ::CreateFontIndirectW(&title);
}

// menu

void App::buildMenu() {
	HMENU bar = ::CreateMenu();

	HMENU file = ::CreatePopupMenu();
	::AppendMenuW(file, MF_STRING, ID_IMPORT, L"&Import .pow...\tCtrl+O");
	::AppendMenuW(file, MF_STRING, ID_EXPORT, L"&Export .pow...\tCtrl+E");
	::AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(file, MF_STRING, ID_EXIT, L"E&xit");
	::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

	HMENU plan = ::CreatePopupMenu();
	::AppendMenuW(plan, MF_STRING, ID_CREATE, L"&New Plan...\tCtrl+N");
	::AppendMenuW(plan, MF_STRING, ID_DUP, L"&Duplicate");
	::AppendMenuW(plan, MF_STRING, ID_RENAME, L"Re&name...\tF2");
	::AppendMenuW(plan, MF_STRING, ID_DESC, L"Edit De&scription...");
	::AppendMenuW(plan, MF_STRING, ID_ACTIVATE, L"Set &Active");
	::AppendMenuW(plan, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(plan, MF_STRING, ID_DELETE, L"De&lete");
	::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(plan), L"&Plan");

	HMENU edit = ::CreatePopupMenu();
	::AppendMenuW(edit, MF_STRING, ID_APPLY, L"&Apply Changes\tCtrl+S");
	::AppendMenuW(edit, MF_STRING, ID_DISCARD, L"Dis&card Changes");
	::AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(edit, MF_STRING, ID_REFRESH, L"&Refresh\tF5");
	::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");

	HMENU tools = ::CreatePopupMenu();
	::AppendMenuW(tools, MF_STRING, ID_DIFF, L"&Compare Plans...");
	::AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(tools, MF_STRING, ID_COPYSEL, L"Copy &Selected From Source\tAlt+Left");
	::AppendMenuW(tools, MF_STRING, ID_COPYALL, L"Copy &All Shown From Source");
	::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"&Tools");

	HMENU help = ::CreatePopupMenu();
	::AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About");
	::AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

	::SetMenu(hwnd_, bar);
}

void App::updateMenuState() {
	HMENU bar = ::GetMenu(hwnd_);
	if (!bar) return;
	const SchemeInfo* s = (selectedScheme_ >= 0 && selectedScheme_ < static_cast<int>(schemeList_.size()))
	                          ? &schemeList_[static_cast<size_t>(selectedScheme_)]
	                          : nullptr;
	auto set = [&](int id, bool on) {
		::EnableMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
	};
	const bool have = s != nullptr;
	set(ID_DUP, have);
	set(ID_RENAME, have);
	set(ID_DESC, have);
	set(ID_EXPORT, have);
	set(ID_ACTIVATE, have && !s->isActive);
	set(ID_DELETE, have && !s->isActive);
	set(ID_APPLY, !pending_.empty() && !applying_);
	set(ID_DISCARD, !pending_.empty() && !applying_);
	const bool canCopy = compareOn_ && compareState_ == LoadState::Ready && !applying_;
	set(ID_COPYSEL, canCopy);
	set(ID_COPYALL, canCopy);
	::DrawMenuBar(hwnd_);
}

void App::showSchemeMenu(int screenX, int screenY) {
	const SchemeInfo* s = (selectedScheme_ >= 0 && selectedScheme_ < static_cast<int>(schemeList_.size()))
	                          ? &schemeList_[static_cast<size_t>(selectedScheme_)]
	                          : nullptr;
	if (!s) return;
	HMENU menu = ::CreatePopupMenu();
	const UINT act = s->isActive ? (MF_STRING | MF_GRAYED) : MF_STRING;
	::AppendMenuW(menu, act, ID_ACTIVATE, L"Set Active");
	::AppendMenuW(menu, MF_STRING, ID_RENAME, L"Rename...");
	::AppendMenuW(menu, MF_STRING, ID_DESC, L"Edit Description...");
	::AppendMenuW(menu, MF_STRING, ID_DUP, L"Duplicate");
	::AppendMenuW(menu, MF_STRING, ID_EXPORT, L"Export .pow...");
	::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	::AppendMenuW(menu, s->isActive ? (MF_STRING | MF_GRAYED) : MF_STRING, ID_DELETE, L"Delete");
	::TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, screenX, screenY, 0, hwnd_, nullptr);
	::DestroyMenu(menu);
}

void App::showSettingMenu(int screenX, int screenY) {
	const std::vector<size_t> sel = selectedSettingIndices();
	if (sel.empty()) return;

	// "Revert" is offered whenever any selected row carries a pending change,
	// so an over-eager copy can be undone row by row without discarding the lot.
	bool anyPending = false;
	for (size_t i : sel) {
		if (i >= enumeration_.settings.size()) continue;
		const SettingEntry& e = enumeration_.settings[i];
		if (pending_.find(e.desc.subgroup, e.desc.id)) {
			anyPending = true;
			break;
		}
	}

	HMENU menu = ::CreatePopupMenu();
	const bool canCopy = compareOn_ && compareState_ == LoadState::Ready && !applying_;
	if (compareOn_) {
		const UINT f = canCopy ? MF_STRING : (MF_STRING | MF_GRAYED);
		const std::wstring n = compareName_.empty() ? L"source" : compareName_;
		::AppendMenuW(menu, f, ID_CTX_COPY, (L"Copy from \"" + n + L"\"\tAlt+Left").c_str());
		::AppendMenuW(menu, f, ID_CTX_COPY_AC, L"Copy AC only");
		::AppendMenuW(menu, f, ID_CTX_COPY_DC, L"Copy DC only");
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	}
	::AppendMenuW(menu, anyPending ? MF_STRING : (MF_STRING | MF_GRAYED), ID_CTX_REVERT,
	              L"Revert pending change");
	::TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, screenX, screenY, 0, hwnd_, nullptr);
	::DestroyMenu(menu);
}

// control creation

HWND App::make(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle) {
	HWND h = ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
	                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
	::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
	return h;
}

void App::createControls() {
	make(L"STATIC", L"Power plans", SS_LEFT, ID_SCHEMES_LABEL);

	HWND schemes =
	    make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOCOLUMNHEADER,
	         ID_SCHEMES, WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(schemes, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	LVCOLUMNW col{};
	col.mask = LVCF_WIDTH;
	col.cx = S(250);
	ListView_InsertColumn(schemes, 0, &col);

	make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, ID_TITLE);
	make(L"STATIC", L"", SS_LEFT, ID_ADVISORY);

	make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_SUBGROUP);
	HWND search = make(L"EDIT", L"", ES_AUTOHSCROLL, ID_SEARCH, WS_EX_CLIENTEDGE);
	::SendMessageW(search, EM_SETCUEBANNER, TRUE,
	               reinterpret_cast<LPARAM>(L"Filter by name, subgroup, or GUID"));
	make(L"BUTTON", L"Hidden only", BS_AUTOCHECKBOX, ID_HIDDEN);
	make(L"BUTTON", L"Modified only", BS_AUTOCHECKBOX, ID_MODIFIED);

	// Compare row. The picker is always visible so the feature is discoverable;
	// the copy actions and the differences filter appear once a source is set.
	make(L"STATIC", L"Compare with:", SS_LEFT, ID_CMPLABEL);
	make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_COMPARE);
	make(L"BUTTON", L"◄ Copy selected", BS_PUSHBUTTON, ID_COPYSEL);
	make(L"BUTTON", L"◄ Copy all shown", BS_PUSHBUTTON, ID_COPYALL);
	make(L"BUTTON", L"Differences only", BS_AUTOCHECKBOX, ID_DIFFONLY);

	// Extended selection (no LVS_SINGLESEL) so a run of rows can be copied in one
	// go; the detail pane follows the focused row, as in Explorer.
	HWND settings =
	    make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDATA, ID_SETTINGS, WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(settings,
	                                  LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
	rebuildSettingColumns();

	detailPanel_ = ::CreateWindowExW(WS_EX_CONTROLPARENT, kDetailClassName, L"",
	                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, hwnd_, nullptr,
	                                 instance_, nullptr);
	::SetWindowLongPtrW(detailPanel_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

	make(L"STATIC", L"", SS_LEFT, ID_PENDLABEL);
	make(L"BUTTON", L"View diff", BS_PUSHBUTTON, ID_VIEWDIFF);
	make(L"BUTTON", L"Discard", BS_PUSHBUTTON, ID_DISCARD);
	make(L"BUTTON", L"Apply", BS_DEFPUSHBUTTON, ID_APPLY);

	HWND status = make(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, ID_STATUS);
	// settings | compare | elevation | standby | build
	int parts[] = {S(300), S(600), S(700), S(860), -1};
	::SendMessageW(status, SB_SETPARTS, 5, reinterpret_cast<LPARAM>(parts));

	(void)schemes;
	(void)settings;
	layout();
}

// The settings grid doubles as the diff surface: in compare mode it grows a
// status glyph column and splits AC/DC into "this plan" and "other plan". The
// Flags column steps aside to make room; its content stays in the detail pane.
void App::rebuildSettingColumns() {
	HWND lv = ::GetDlgItem(hwnd_, ID_SETTINGS);
	if (!lv) return;

	// Empty the virtual list first: a repaint against a half-rebuilt column set
	// would ask LVN_GETDISPINFO for subitems that no longer exist. rebuildFilter
	// restores the count.
	ListView_SetItemCount(lv, 0);

	// Drop the old set from the right so the indices stay valid as we go.
	HWND header = ListView_GetHeader(lv);
	for (int i = Header_GetItemCount(header) - 1; i >= 0; --i)
		ListView_DeleteColumn(lv, i);

	struct Col {
		const wchar_t* title;
		int width;
	};
	static const Col plain[] = {
	    {L"Setting", 240}, {L"Subgroup", 170}, {L"AC", 80}, {L"DC", 80}, {L"Flags", 150}};
	static const Col compare[] = {{L"Δ", 26},         {L"Setting", 180},   {L"Subgroup", 110},
	                              {L"AC (this)", 72}, {L"AC (other)", 72}, {L"DC (this)", 72},
	                              {L"DC (other)", 72}};

	const Col* cols = compareOn_ ? compare : plain;
	const int n = compareOn_ ? static_cast<int>(std::size(compare)) : static_cast<int>(std::size(plain));
	for (int i = 0; i < n; ++i) {
		LVCOLUMNW c{};
		c.mask = LVCF_TEXT | LVCF_WIDTH;
		c.pszText = const_cast<wchar_t*>(cols[i].title);
		c.cx = S(cols[i].width);
		ListView_InsertColumn(lv, i, &c);
	}
	compareColumns_ = compareOn_;
	::InvalidateRect(lv, nullptr, TRUE);
}

void App::layout() {
	RECT rc{};
	::GetClientRect(hwnd_, &rc);
	const int W = rc.right, H = rc.bottom;
	const int pad = S(8);
	const int rowH = S(26);
	const int gap = S(6);
	const int leftW = S(264);
	const int detailW = S(340);
	const int statusH = S(24);
	const int pendH = S(36);

	auto place = [&](int id, int x, int y, int w, int h) {
		::SetWindowPos(::GetDlgItem(hwnd_, id), nullptr, x, y, w, h, SWP_NOZORDER);
	};

	HWND status = ::GetDlgItem(hwnd_, ID_STATUS);
	::SendMessageW(status, WM_SIZE, 0, 0);
	const int statusTop = H - statusH;

	const bool pendingVisible = !pending_.empty();
	const int pendTop = statusTop - (pendingVisible ? pendH : 0);

	// Left panel: plan list fills the full height (actions are on the menu).
	const int lx = pad;
	int y = pad;
	place(ID_SCHEMES_LABEL, lx, y, leftW, S(16));
	y += S(18);
	place(ID_SCHEMES, lx, y, leftW, pendTop - pad - y);

	// Right area.
	const int rx = lx + leftW + pad;
	const int rw = W - rx - pad;
	y = pad;

	// Header line: selected plan name and GUID.
	place(ID_TITLE, rx, y + S(2), rw, S(18));
	y += S(20);

	// Modern Standby advisory, when applicable.
	HWND advisory = ::GetDlgItem(hwnd_, ID_ADVISORY);
	const bool advVisible = isModernStandby();
	::ShowWindow(advisory, advVisible ? SW_SHOW : SW_HIDE);
	if (advVisible) {
		place(ID_ADVISORY, rx, y, rw, S(30));
		y += S(30) + gap;
	}

	// Filter row.
	const int subW = S(200);
	const int chkW = S(104);
	place(ID_SUBGROUP, rx, y, subW, S(200)); // combo height is the dropdown extent
	place(ID_MODIFIED, rx + rw - chkW, y + S(3), chkW, S(20));
	place(ID_HIDDEN, rx + rw - chkW * 2 - gap, y + S(3), chkW, S(20));
	place(ID_SEARCH, rx + subW + gap, y, rw - subW - gap - chkW * 2 - gap * 2, rowH);
	y += rowH + gap;

	// Compare row: source picker, the two copy actions, and the diff filter.
	const int cmpLabelW = S(78);
	const int cmpComboW = S(190);
	place(ID_CMPLABEL, rx, y + S(5), cmpLabelW, S(16));
	place(ID_COMPARE, rx + cmpLabelW, y, cmpComboW, S(240));
	if (compareOn_) {
		const int selW = S(118), allW = S(124);
		int cx = rx + cmpLabelW + cmpComboW + gap * 2;
		place(ID_COPYSEL, cx, y, selW, rowH);
		cx += selW + gap;
		place(ID_COPYALL, cx, y, allW, rowH);
		place(ID_DIFFONLY, rx + rw - chkW, y + S(3), chkW, S(20));
	}
	for (int id : {ID_COPYSEL, ID_COPYALL, ID_DIFFONLY}) {
		::ShowWindow(::GetDlgItem(hwnd_, id), compareOn_ ? SW_SHOW : SW_HIDE);
	}
	y += rowH + gap;

	// Table + detail split.
	const int splitTop = y;
	const int splitBottom = pendTop - pad;
	const int tableW = rw - detailW - pad;
	place(ID_SETTINGS, rx, splitTop, tableW, splitBottom - splitTop);
	::SetWindowPos(detailPanel_, nullptr, rx + tableW + pad, splitTop, detailW, splitBottom - splitTop,
	               SWP_NOZORDER);

	// Pending bar.
	if (pendingVisible) {
		const int pbW = S(90);
		int px = W - pad;
		place(ID_APPLY, px - pbW, pendTop + S(2), pbW, rowH);
		px -= pbW + gap;
		place(ID_DISCARD, px - pbW, pendTop + S(2), pbW, rowH);
		px -= pbW + gap;
		place(ID_VIEWDIFF, px - pbW, pendTop + S(2), pbW, rowH);
		place(ID_PENDLABEL, rx, pendTop + S(8), px - pbW - rx - gap, S(20));
	}
	for (int id : {ID_APPLY, ID_DISCARD, ID_VIEWDIFF, ID_PENDLABEL}) {
		::ShowWindow(::GetDlgItem(hwnd_, id), pendingVisible ? SW_SHOW : SW_HIDE);
	}

	layoutDetail();
	::InvalidateRect(hwnd_, nullptr, FALSE);
}

// Re-stacks the detail controls inside the panel and syncs its scrollbar. The
// stack can run past the panel bottom (long description + both rails + compare
// source rows), and a "Use this value" button you cannot reach is a dead
// feature, so the panel scrolls rather than clipping.
void App::layoutDetail() {
	if (!detailPanel_) return;
	RECT dp{};
	::GetClientRect(detailPanel_, &dp);

	int content = S(4);
	for (size_t i = 0; i < detailControls_.size(); ++i)
		content += detailHeights_[i] + S(4);

	// Set the scrollbar first: showing or hiding it changes the client width, and
	// the children must be sized against the width they will actually have.
	// Content height does not depend on width here, so one pass settles it.
	// Without SIF_DISABLENOSCROLL the bar hides itself when everything fits.
	SCROLLINFO si{sizeof(si)};
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = content > 0 ? content - 1 : 0;
	si.nPage = static_cast<UINT>(std::max<LONG>(dp.bottom, 0));
	si.nPos = detailScroll_;
	::SetScrollInfo(detailPanel_, SB_VERT, &si, TRUE);

	::GetClientRect(detailPanel_, &dp);
	const int maxScroll = std::max<int>(0, content - static_cast<int>(dp.bottom));
	detailScroll_ = std::clamp(detailScroll_, 0, maxScroll);

	int dy = S(4) - detailScroll_;
	for (size_t i = 0; i < detailControls_.size(); ++i) {
		::SetWindowPos(detailControls_[i], nullptr, S(6), dy, dp.right - S(12), detailHeights_[i],
		               SWP_NOZORDER);
		dy += detailHeights_[i] + S(4);
	}
	if (si.nPos != detailScroll_) ::SetScrollPos(detailPanel_, SB_VERT, detailScroll_, TRUE);
}

void App::onDetailScroll(int action, int thumbPos) {
	RECT dp{};
	::GetClientRect(detailPanel_, &dp);
	const int line = S(18), page = std::max<int>(dp.bottom - S(18), line);
	const int before = detailScroll_;
	switch (action) {
	case SB_LINEUP: detailScroll_ -= line; break;
	case SB_LINEDOWN: detailScroll_ += line; break;
	case SB_PAGEUP: detailScroll_ -= page; break;
	case SB_PAGEDOWN: detailScroll_ += page; break;
	case SB_TOP: detailScroll_ = 0; break;
	case SB_BOTTOM: detailScroll_ = 1 << 24; break; // clamped by layoutDetail
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: detailScroll_ = thumbPos; break;
	default: return;
	}
	layoutDetail(); // clamps detailScroll_ and re-syncs the bar
	if (detailScroll_ != before) ::InvalidateRect(detailPanel_, nullptr, TRUE);
}

void App::onDetailWheel(int delta) {
	UINT lines = 3;
	::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
	if (lines == 0) return; // wheel scrolling turned off system-wide
	const int before = detailScroll_;
	detailScroll_ -= delta * S(18) * static_cast<int>(lines) / WHEEL_DELTA;
	layoutDetail();
	if (detailScroll_ != before) ::InvalidateRect(detailPanel_, nullptr, TRUE);
}

// data / worker

void App::loadSchemes(std::optional<GUID> preferred) {
	auto result = schemes_.enumerateSchemes();
	if (!result) {
		showError(L"Could not enumerate power schemes", result.error());
		enumState_ = LoadState::Error;
		return;
	}
	schemeList_ = std::move(*result);
	// Select the preferred scheme (e.g. one just imported/created); otherwise
	// fall back to the active scheme, then the first.
	selectedScheme_ = schemeList_.empty() ? -1 : 0;
	bool found = false;
	if (preferred) {
		for (size_t i = 0; i < schemeList_.size(); ++i) {
			if (guidEqual(schemeList_[i].id, *preferred)) {
				selectedScheme_ = static_cast<int>(i);
				found = true;
				break;
			}
		}
	}
	if (!found) {
		for (size_t i = 0; i < schemeList_.size(); ++i) {
			if (schemeList_[i].isActive) {
				selectedScheme_ = static_cast<int>(i);
				break;
			}
		}
	}
	refreshSchemeList();
	startEnumeration();
}

void App::refreshSchemeList() {
	HWND lv = ::GetDlgItem(hwnd_, ID_SCHEMES);
	// Rebuilding the list fires LVN_ITEMCHANGED selection events; ignore them
	// so they don't hijack selectedScheme_ (only real user clicks should).
	suppressSchemeSelect_ = true;
	ListView_DeleteAllItems(lv);
	for (size_t i = 0; i < schemeList_.size(); ++i) {
		std::wstring text = schemeList_[i].name;
		if (schemeList_[i].isActive) text += L"  [active]";
		if (schemeList_[i].isOurs) text += L"  [created here]";
		LVITEMW it{};
		it.mask = LVIF_TEXT;
		it.iItem = static_cast<int>(i);
		it.pszText = text.data();
		ListView_InsertItem(lv, &it);
	}
	if (selectedScheme_ >= 0) {
		ListView_SetItemState(lv, selectedScheme_, LVIS_SELECTED | LVIS_FOCUSED,
		                      LVIS_SELECTED | LVIS_FOCUSED);
	}
	suppressSchemeSelect_ = false;
	rebuildCompareCombo();
	updateStatus();
}

// Lists every plan except the one being edited. The current source is tracked by
// GUID, so it survives renames and list reloads; if the plan is gone (deleted
// elsewhere, or now the edit target) compare mode switches off.
void App::rebuildCompareCombo() {
	HWND combo = ::GetDlgItem(hwnd_, ID_COMPARE);
	if (!combo) return;
	::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(none)"));
	compareCombo_.clear();

	int sel = 0;
	for (size_t i = 0; i < schemeList_.size(); ++i) {
		if (selectedScheme_ >= 0 && i == static_cast<size_t>(selectedScheme_)) continue;
		::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(schemeList_[i].name.c_str()));
		compareCombo_.push_back(i);
		if (compareOn_ && guidEqual(schemeList_[i].id, compareSource_)) {
			sel = static_cast<int>(compareCombo_.size()); // +1 for "(none)"
			compareName_ = schemeList_[i].name;
		}
	}
	::SendMessageW(combo, CB_SETCURSEL, sel, 0);
	if (compareOn_ && sel == 0) setCompareSource(std::nullopt); // source disappeared
}

void App::setCompareSource(std::optional<GUID> source) {
	const bool wasOn = compareOn_;
	compareOn_ = source.has_value();
	compareSource_ = source.value_or(GUID{});
	compareEnum_ = SchemeEnumeration{};
	compareIndex_.clear();
	compareState_ = LoadState::Loading;
	++compareGeneration_; // abandon any in-flight read for the previous source

	if (!compareOn_) {
		compareName_.clear();
		filterDiffOnly_ = false;
		::SendDlgItemMessageW(hwnd_, ID_DIFFONLY, BM_SETCHECK, BST_UNCHECKED, 0);
	} else {
		compareName_.clear();
		for (const auto& s : schemeList_) {
			if (guidEqual(s.id, compareSource_)) {
				compareName_ = s.name;
				break;
			}
		}
	}

	if (wasOn != compareOn_) rebuildSettingColumns();
	rebuildCompareCombo();
	if (compareOn_) startCompareEnumeration();
	rebuildFilter();
	rebuildDetail();
	updateStatus();
	layout();
}

void App::startCompareEnumeration() {
	if (!compareOn_) return;
	const GUID scheme = compareSource_;
	const unsigned generation = compareGeneration_;
	HWND hwnd = hwnd_;
	// Off the UI thread, like the primary enumeration: reading a plan takes long
	// enough to stutter the window if done inline.
	WorkerQueue::shared().post([this, hwnd, scheme, generation] {
		auto r = enumerator_.enumerateScheme(scheme);
		auto* res = new EnumResult{};
		res->generation = generation;
		if (r) {
			res->ok = true;
			res->enumeration = std::move(*r);
		} else {
			res->error = r.error().describe();
		}
		::PostMessageW(hwnd, WM_APP_CMP_DONE, generation, reinterpret_cast<LPARAM>(res));
	});
}

void App::onCompareDone(EnumResult* res) {
	std::unique_ptr<EnumResult> guard(res);
	if (!compareOn_ || res->generation != compareGeneration_) return; // superseded
	if (!res->ok) {
		compareState_ = LoadState::Error;
		updateStatus();
		return;
	}
	compareEnum_ = std::move(res->enumeration);
	compareState_ = LoadState::Ready;

	// Index the source once; lookups happen per visible row and per filter pass.
	compareIndex_.clear();
	for (size_t i = 0; i < compareEnum_.settings.size(); ++i) {
		const SettingDescriptor& d = compareEnum_.settings[i].desc;
		compareIndex_.emplace(SettingKey{d.subgroup, d.id}, i);
	}
	recomputeSourceOnly();

	rebuildFilter();
	rebuildDetail();
	updateStatus();
}

const SettingEntry* App::sourceEntry(const GUID& sub, const GUID& id) const {
	if (compareState_ != LoadState::Ready) return nullptr;
	auto it = compareIndex_.find(SettingKey{sub, id});
	return it == compareIndex_.end() ? nullptr : &compareEnum_.settings[it->second];
}

// Compares effective (pending-inclusive) values, so the glyph always reflects
// what Apply would produce rather than what is currently on disk.
DiffState App::diffStateOf(const SettingEntry& e) const {
	if (pending_.find(e.desc.subgroup, e.desc.id)) return DiffState::Staged;
	const SettingEntry* src = sourceEntry(e.desc.subgroup, e.desc.id);
	if (!src) return DiffState::OnlyHere;
	const auto ac = effectiveValue(e, true), dc = effectiveValue(e, false);
	if (!ac && !dc && (src->values.ac || src->values.dc)) return DiffState::OnlyThere;
	return (ac == src->values.ac && dc == src->values.dc) ? DiffState::Same : DiffState::Differs;
}

size_t App::differenceCount() const {
	if (!compareOn_ || compareState_ != LoadState::Ready) return 0;
	size_t n = 0;
	for (const auto& e : enumeration_.settings)
		if (diffStateOf(e) != DiffState::Same) ++n;
	return n;
}

// The grid is driven by the edited plan's enumeration, so a setting the source
// has and this plan does not has no row to appear on. Rare (it means the two
// plans enumerate different settings), but silently dropping it would make the
// grid look complete when it is not, so the count goes in the status bar and
// Tools > Compare Plans still lists them individually.
void App::recomputeSourceOnly() {
	sourceOnly_ = 0;
	if (!compareOn_ || compareState_ != LoadState::Ready) return;
	std::map<SettingKey, bool, SettingKeyLess> here;
	for (const auto& e : enumeration_.settings)
		here.emplace(SettingKey{e.desc.subgroup, e.desc.id}, true);
	for (const auto& e : compareEnum_.settings)
		if (!here.count(SettingKey{e.desc.subgroup, e.desc.id})) ++sourceOnly_;
}

const GUID* App::selectedSchemeGuid() const {
	if (selectedScheme_ < 0 || selectedScheme_ >= static_cast<int>(schemeList_.size())) return nullptr;
	return &schemeList_[static_cast<size_t>(selectedScheme_)].id;
}

void App::startEnumeration() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const GUID scheme = *g;
	const unsigned generation = ++enumGeneration_;
	enumState_ = LoadState::Loading;
	const SchemeInfo& si = schemeList_[static_cast<size_t>(selectedScheme_)];
	std::wstring header = si.name + L"    " + guidToString(si.id);
	if (si.isActive) header += L"    [active]";
	::SetDlgItemTextW(hwnd_, ID_TITLE, header.c_str());
	ListView_SetItemCount(::GetDlgItem(hwnd_, ID_SETTINGS), 0);
	clearDetail();
	updateStatus();

	HWND hwnd = hwnd_;
	WorkerQueue::shared().post([this, hwnd, scheme, generation] {
		auto r = enumerator_.enumerateScheme(scheme);
		auto* res = new EnumResult{};
		res->generation = generation;
		if (r) {
			res->ok = true;
			res->enumeration = std::move(*r);
		} else {
			res->error = r.error().describe();
		}
		::PostMessageW(hwnd, WM_APP_ENUM_DONE, generation, reinterpret_cast<LPARAM>(res));
	});
}

void App::onEnumDone(EnumResult* res) {
	std::unique_ptr<EnumResult> guard(res);
	if (res->generation != enumGeneration_) return; // superseded
	if (!res->ok) {
		enumState_ = LoadState::Error;
		enumError_ = res->error;
		updateStatus();
		return;
	}
	// Capture the chosen subgroup before the old enumeration goes away.
	std::optional<GUID> keepSub;
	if (selectedSubgroup_ > 0 &&
	    static_cast<size_t>(selectedSubgroup_ - 1) < enumeration_.subgroups.size())
		keepSub = enumeration_.subgroups[static_cast<size_t>(selectedSubgroup_ - 1)].id;

	enumeration_ = std::move(res->enumeration);
	enumState_ = LoadState::Ready;
	recomputeSourceOnly(); // the target side of the comparison just changed
	rebuildSubgroupCombo(keepSub);
	rebuildFilter();
	updateStatus();
}

void App::rebuildSubgroupCombo(std::optional<GUID> keep) {
	HWND combo = ::GetDlgItem(hwnd_, ID_SUBGROUP);
	::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All subgroups"));
	// The subgroup scopes "Copy all shown", so it must survive the re-enumeration
	// that fires on every window activation: silently widening back to "All
	// subgroups" would turn a deliberately narrow copy into a whole-plan one.
	selectedSubgroup_ = 0;
	for (size_t i = 0; i < enumeration_.subgroups.size(); ++i) {
		const auto& sg = enumeration_.subgroups[i];
		::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sg.name.c_str()));
		if (keep && guidEqual(sg.id, *keep)) selectedSubgroup_ = static_cast<int>(i) + 1;
	}
	::SendMessageW(combo, CB_SETCURSEL, selectedSubgroup_, 0);
}

const SettingEntry* App::rowEntry(size_t row) const {
	if (row >= filtered_.size()) return nullptr;
	const size_t idx = filtered_[row];
	return idx < enumeration_.settings.size() ? &enumeration_.settings[idx] : nullptr;
}

const SettingEntry* App::findEntry(const GUID& sub, const GUID& id) const {
	for (const auto& s : enumeration_.settings)
		if (guidEqual(s.desc.subgroup, sub) && guidEqual(s.desc.id, id)) return &s;
	return nullptr;
}

std::wstring App::subgroupName(const GUID& id) const {
	for (const auto& sg : enumeration_.subgroups) {
		if (guidEqual(sg.id, id)) return sg.name;
	}
	return guidToString(id);
}

bool App::isModified(const SettingEntry& e) const {
	if (pending_.find(e.desc.subgroup, e.desc.id)) return true;
	const bool ac = e.values.ac && e.values.acDefault && *e.values.ac != *e.values.acDefault;
	const bool dc = e.values.dc && e.values.dcDefault && *e.values.dc != *e.values.dcDefault;
	return ac || dc;
}

std::optional<DWORD> App::effectiveValue(const SettingEntry& e, bool ac) const {
	if (const PendingChange* c = pending_.find(e.desc.subgroup, e.desc.id)) {
		if (ac && c->acNew) return c->acNew;
		if (!ac && c->dcNew) return c->dcNew;
	}
	return ac ? e.values.ac : e.values.dc;
}

void App::rebuildFilter() {
	// Remember the focused setting by identity; row indices do not survive a
	// filter change or a re-enumeration. startEnumeration empties the list before
	// the worker replies, so fall back to whatever the detail pane last showed.
	std::optional<SettingKey> keep = detailKey_;
	if (const int row = selectedSettingRow(); row >= 0) {
		if (const SettingEntry* cur = rowEntry(static_cast<size_t>(row)))
			keep = SettingKey{cur->desc.subgroup, cur->desc.id};
	}

	filtered_.clear();
	for (size_t i = 0; i < enumeration_.settings.size(); ++i) {
		const SettingEntry& e = enumeration_.settings[i];
		if (selectedSubgroup_ > 0) {
			const size_t sg = static_cast<size_t>(selectedSubgroup_) - 1;
			if (sg >= enumeration_.subgroups.size() ||
			    !guidEqual(e.desc.subgroup, enumeration_.subgroups[sg].id))
				continue;
		}
		if (filterHidden_ && !e.desc.hidden) continue;
		if (filterModified_ && !isModified(e)) continue;
		// Rows stay put once copied (Staged, not Same) so the list does not
		// shift under the cursor mid-review; they drop out after Apply.
		if (filterDiffOnly_ && compareOn_ && diffStateOf(e) == DiffState::Same) continue;
		if (!filterText_.empty()) {
			if (!containsNoCase(e.desc.name, filterText_) &&
			    !containsNoCase(subgroupName(e.desc.subgroup), filterText_) &&
			    !containsNoCase(guidToString(e.desc.id), filterText_))
				continue;
		}
		filtered_.push_back(i);
	}
	HWND lv = ::GetDlgItem(hwnd_, ID_SETTINGS);
	ListView_SetItemCount(lv, static_cast<int>(filtered_.size()));
	if (!filtered_.empty()) {
		// Land back on the same setting where possible. Re-enumeration happens on
		// every window activation, and snapping to the top each time would make
		// alt-tabbing between plans unusable.
		int row = 0;
		if (keep) {
			for (size_t i = 0; i < filtered_.size(); ++i) {
				const SettingEntry& e = enumeration_.settings[filtered_[i]];
				if (guidEqual(e.desc.subgroup, keep->sub) && guidEqual(e.desc.id, keep->id)) {
					row = static_cast<int>(i);
					break;
				}
			}
		}
		ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED); // drop any stale multi-selection
		ListView_SetItemState(lv, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(lv, row, FALSE);
	} else {
		clearDetail();
	}
	::InvalidateRect(lv, nullptr, FALSE);
	updateStatus();
}

// detail pane

void App::clearDetail() {
	for (HWND h : detailControls_)
		::DestroyWindow(h);
	detailControls_.clear();
	detailHeights_.clear();
	editors_.clear();
}

HWND App::addDetailLine(const wchar_t* cls, const wchar_t* text, DWORD style, int height, DWORD exStyle) {
	HWND h = ::CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, detailPanel_,
	                           nullptr, instance_, nullptr);
	::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
	detailControls_.push_back(h);
	detailHeights_.push_back(height);
	return h;
}

void App::addRailEditor(const SettingEntry& e, bool ac) {
	const SettingDescriptor& d = e.desc;
	const bool locked = ac ? d.acLockedByPolicy : d.dcLockedByPolicy;
	const auto current = effectiveValue(e, ac);

	const std::wstring railBase = ac ? L"Plugged in (AC)" : L"On battery (DC)";
	std::wstring headerText = railBase;
	if (current) headerText += L":  " + d.renderValue(current);
	if (locked) headerText += L"   [locked by policy]";
	HWND header = addDetailLine(L"STATIC", headerText.c_str(), SS_LEFT, S(18));

	if (!current) {
		addDetailLine(L"STATIC", L"No value on this rail.", SS_LEFT, S(16));
		// Still worth offering: the source may have a value this plan lacks.
		addCompareRail(e, ac, locked, current);
		return;
	}

	const GUID sub = d.subgroup, id = d.id;

	if (d.kind == SettingKind::Enum) {
		HWND combo = addDetailLine(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, S(220));
		// Height arg here is the dropped-down extent; the closed control is
		// sized by the combo itself. Store a real closed height for stacking.
		detailHeights_.back() = S(24);
		Editor ed;
		ed.type = Editor::Combo;
		ed.sub = sub;
		ed.setting = id;
		ed.ac = ac;
		size_t sel = 0;
		for (size_t i = 0; i < d.possibleValues.size(); ++i) {
			const std::wstring name = d.possibleValues[i].name.empty()
			                              ? std::to_wstring(d.possibleValues[i].index)
			                              : d.possibleValues[i].name;
			::SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
			ed.enumValues.push_back(d.possibleValues[i].index);
			if (d.possibleValues[i].index == *current) sel = i;
		}
		::SendMessageW(combo, CB_SETCURSEL, sel, 0);
		::EnableWindow(combo, !locked);
		editors_[combo] = std::move(ed);
	} else if (d.kind == SettingKind::Range && d.units == L"%" && d.max <= 100) {
		HWND bar = addDetailLine(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, S(28));
		::SendMessageW(bar, TBM_SETRANGEMIN, TRUE, d.min);
		::SendMessageW(bar, TBM_SETRANGEMAX, TRUE, d.max);
		::SendMessageW(bar, TBM_SETPAGESIZE, 0, d.increment ? d.increment : 1);
		::SendMessageW(bar, TBM_SETPOS, TRUE, static_cast<LPARAM>(*current));
		::EnableWindow(bar, !locked);
		Editor ed;
		ed.type = Editor::Trackbar;
		ed.sub = sub;
		ed.setting = id;
		ed.ac = ac;
		ed.header = header;
		ed.headerBase = railBase;
		editors_[bar] = std::move(ed);
	} else {
		HWND edit = addDetailLine(L"EDIT", std::to_wstring(*current).c_str(), ES_AUTOHSCROLL | ES_NUMBER,
		                          S(24), WS_EX_CLIENTEDGE);
		HWND ud = ::CreateWindowExW(0, UPDOWN_CLASSW, L"",
		                            WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS |
		                                UDS_NOTHOUSANDS,
		                            0, 0, 0, 0, detailPanel_, nullptr, instance_, nullptr);
		detailControls_.push_back(ud);
		detailHeights_.push_back(0); // laid over the edit; not stacked
		::SendMessageW(ud, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);
		const DWORD minV = d.kind == SettingKind::Opaque ? 0u : d.min;
		const DWORD maxV = d.kind == SettingKind::Opaque ? 0x7FFFFFFFu : d.max;
		::SendMessageW(ud, UDM_SETRANGE32, static_cast<WPARAM>(minV), static_cast<LPARAM>(maxV));
		::SendMessageW(ud, UDM_SETPOS32, 0, static_cast<LPARAM>(*current));
		::EnableWindow(edit, !locked);
		::EnableWindow(ud, !locked);
		Editor ed;
		ed.type = Editor::Spinner;
		ed.sub = sub;
		ed.setting = id;
		ed.ac = ac;
		editors_[edit] = std::move(ed);
	}

	const auto def = ac ? e.values.acDefault : e.values.dcDefault;
	if (def) {
		std::wstring defText = L"Default: " + d.renderValue(def);
		addDetailLine(L"STATIC", defText.c_str(), SS_LEFT, S(16));
		if (!locked && current != def) {
			HWND reset = addDetailLine(L"BUTTON", L"Reset to default", BS_PUSHBUTTON, S(24));
			Editor ed;
			ed.type = Editor::Reset;
			ed.sub = sub;
			ed.setting = id;
			ed.ac = ac;
			ed.actionValue = *def;
			editors_[reset] = std::move(ed);
		}
	}

	addCompareRail(e, ac, locked, current);
}

// Shows the compare source's value for this rail and, when it differs and can
// be written, a one-click adopt. This is the precise counterpart to the grid's
// bulk copy: full context (name, range, description) is on screen.
void App::addCompareRail(const SettingEntry& e, bool ac, bool locked, std::optional<DWORD> current) {
	if (!compareOn_) return;
	const SettingDescriptor& d = e.desc;
	const std::wstring who = compareName_.empty() ? L"Source" : compareName_;

	if (compareState_ != LoadState::Ready) {
		addDetailLine(L"STATIC",
		              (who + (compareState_ == LoadState::Error ? L": could not be read" : L": reading..."))
		                  .c_str(),
		              SS_LEFT, S(16));
		return;
	}

	const SettingEntry* src = sourceEntry(d.subgroup, d.id);
	if (!src) {
		addDetailLine(L"STATIC", (who + L": setting not present").c_str(), SS_LEFT, S(16));
		return;
	}
	const auto sv = ac ? src->values.ac : src->values.dc;
	if (!sv) {
		addDetailLine(L"STATIC", (who + L": no value on this rail").c_str(), SS_LEFT, S(16));
		return;
	}

	addDetailLine(L"STATIC", (who + L":  " + d.renderValue(sv)).c_str(), SS_LEFT, S(16));
	if (locked) {
		addDetailLine(L"STATIC", L"Cannot copy: locked by policy.", SS_LEFT, S(16));
		return;
	}
	if (current && *current == *sv) return; // nothing to adopt
	// No validateValue gate here either: it hid this button behind "not a valid
	// value here" for values that write fine. See stageFromSource.

	HWND adopt = addDetailLine(L"BUTTON", L"◄ Use this value", BS_PUSHBUTTON, S(24));
	Editor ed;
	ed.type = Editor::Adopt;
	ed.sub = d.subgroup;
	ed.setting = d.id;
	ed.ac = ac;
	ed.actionValue = *sv;
	editors_[adopt] = std::move(ed);
}

void App::rebuildDetail() {
	clearDetail();
	const int row = selectedSettingRow();
	const SettingEntry* e = row >= 0 ? rowEntry(static_cast<size_t>(row)) : nullptr;

	// Keep the scroll position while the same setting is on screen (adopting a
	// value rebuilds the pane, and snapping back to the top each time would fight
	// anyone working on the DC rail); reset it when the setting changes.
	std::optional<SettingKey> key;
	if (e) key = SettingKey{e->desc.subgroup, e->desc.id};
	const bool same = key.has_value() == detailKey_.has_value() &&
	                  (!key || (guidEqual(key->sub, detailKey_->sub) && guidEqual(key->id, detailKey_->id)));
	if (!same) detailScroll_ = 0;
	detailKey_ = key;

	if (!e) {
		if (!enumeration_.settings.empty()) {
			addDetailLine(L"STATIC", L"Select a setting to edit it.", SS_LEFT, S(18));
		}
		layout();
		return;
	}
	const SettingDescriptor& d = e->desc;

	HWND title = addDetailLine(L"STATIC", d.name.c_str(), SS_LEFT, S(24));
	::SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(fontTitle_), TRUE);

	std::wstring flags;
	if (d.hidden) flags += L"Hidden   ";
	if (d.acLockedByPolicy || d.dcLockedByPolicy) flags += L"Policy-locked   ";
	if (d.metadataIncomplete) flags += L"Raw index   ";
	if (!flags.empty()) addDetailLine(L"STATIC", flags.c_str(), SS_LEFT, S(16));

	if (!d.description.empty()) {
		// Compute a wrapped height for the description. Measure against the
		// narrower with-scrollbar width so the text never comes out taller than
		// the space reserved for it.
		RECT r{0, 0, S(340) - S(12) - ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_), S(400)};
		HDC dc = ::GetDC(detailPanel_);
		HGDIOBJ old = ::SelectObject(dc, font_);
		::DrawTextW(dc, d.description.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK);
		::SelectObject(dc, old);
		::ReleaseDC(detailPanel_, dc);
		addDetailLine(L"STATIC", d.description.c_str(), SS_LEFT, std::max<long>(r.bottom, S(16)));
	}

	HWND guid = addDetailLine(L"BUTTON", (guidToString(d.id) + L"   (copy)").c_str(), BS_PUSHBUTTON, S(24));
	Editor ged;
	ged.type = Editor::GuidCopy;
	ged.guidText = guidToString(d.id);
	editors_[guid] = std::move(ged);

	std::wstring kind;
	if (d.kind == SettingKind::Range) {
		kind = L"Range " + std::to_wstring(d.min) + L" to " + std::to_wstring(d.max) + L", step " +
		       std::to_wstring(d.increment);
		if (!d.units.empty()) kind += L" " + d.units;
	} else if (d.kind == SettingKind::Enum) {
		kind = std::to_wstring(d.possibleValues.size()) + L" possible values";
	} else {
		kind = L"No range or enum metadata, raw index only";
	}
	addDetailLine(L"STATIC", kind.c_str(), SS_LEFT, S(16));

	addRailEditor(*e, true);
	addRailEditor(*e, false);
	layout();
}

// The detail pane follows the focused row, not the selection, so it stays put
// while a multi-row selection is being extended.
int App::selectedSettingRow() const {
	return ListView_GetNextItem(::GetDlgItem(hwnd_, ID_SETTINGS), -1, LVNI_FOCUSED);
}

// Selected rows as indices into enumeration_.settings. Falls back to the focused
// row so the copy actions still do something sensible with nothing selected.
std::vector<size_t> App::selectedSettingIndices() const {
	std::vector<size_t> out;
	HWND lv = ::GetDlgItem(hwnd_, ID_SETTINGS);
	for (int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); i >= 0;
	     i = ListView_GetNextItem(lv, i, LVNI_SELECTED)) {
		if (static_cast<size_t>(i) < filtered_.size()) out.push_back(filtered_[static_cast<size_t>(i)]);
	}
	if (out.empty()) {
		const int f = selectedSettingRow();
		if (f >= 0 && static_cast<size_t>(f) < filtered_.size()) out.push_back(filtered_[static_cast<size_t>(f)]);
	}
	return out;
}

// editor events

void App::onEditorCommand(HWND ctrl, int code) {
	auto it = editors_.find(ctrl);
	if (it == editors_.end()) return;
	Editor& ed = it->second;
	switch (ed.type) {
	case Editor::Combo:
		if (code == CBN_SELCHANGE) {
			const int sel = static_cast<int>(::SendMessageW(ctrl, CB_GETCURSEL, 0, 0));
			if (sel >= 0 && sel < static_cast<int>(ed.enumValues.size())) {
				stageValue(ed.sub, ed.setting, ed.ac, ed.enumValues[sel]);
			}
		}
		break;
	case Editor::Spinner:
		if (code == EN_CHANGE) {
			wchar_t buf[32];
			::GetWindowTextW(ctrl, buf, 32);
			if (buf[0]) {
				wchar_t* end = nullptr;
				const unsigned long v = ::wcstoul(buf, &end, 10);
				if (end && *end == L'\0') stageValue(ed.sub, ed.setting, ed.ac, static_cast<DWORD>(v));
			}
		}
		break;
	case Editor::Reset:
	case Editor::Adopt: {
		// Copy the fields out: staging repaints the grid, and rebuildDetail below
		// destroys the editor map this reference points into.
		const GUID sub = ed.sub, setting = ed.setting;
		const bool ac = ed.ac;
		const DWORD value = ed.actionValue;
		// validate=false: both the plan default and the source's value came from
		// Windows, so they are attested rather than typed.
		stageValue(sub, setting, ac, value, /*validate=*/false); // also repaints the grid
		updateStatus();                                          // the difference count moved
		// Not rebuildFilter(): re-selecting rows from in here would re-enter
		// rebuildDetail while this button's own click is still on the stack.
		// The row stays visible under "Differences only" either way.
		rebuildDetail();
		break;
	}
	case Editor::GuidCopy: copyToClipboard(ed.guidText); break;
	case Editor::Trackbar: break;
	}
}

void App::onTrackbar(HWND ctrl) {
	auto it = editors_.find(ctrl);
	if (it == editors_.end()) return;
	Editor& ed = it->second;
	const DWORD pos = static_cast<DWORD>(::SendMessageW(ctrl, TBM_GETPOS, 0, 0));
	stageValue(ed.sub, ed.setting, ed.ac, pos);
	// Update the rail header static with the live value.
	const SettingEntry* e = findEntry(ed.sub, ed.setting);
	if (e && ed.header) {
		std::wstring t = ed.headerBase + L":  " + e->desc.renderValue(pos);
		::SetWindowTextW(ed.header, t.c_str());
	}
}

// status / pending

void App::updateStatus() {
	HWND status = ::GetDlgItem(hwnd_, ID_STATUS);
	std::wstring s;
	switch (enumState_) {
	case LoadState::Loading: s = L"Enumerating..."; break;
	case LoadState::Error: s = L"Enumeration failed, see log"; break;
	case LoadState::Ready:
		s = std::to_wstring(enumeration_.settings.size()) + L" settings";
		if (filtered_.size() != enumeration_.settings.size())
			s += L" (" + std::to_wstring(filtered_.size()) + L" shown)";
		if (!enumeration_.warnings.empty())
			s += L", " + std::to_wstring(enumeration_.warnings.size()) + L" read warning(s)";
		break;
	}
	std::wstring cmp;
	if (compareOn_) {
		switch (compareState_) {
		case LoadState::Loading: cmp = L"Reading \"" + compareName_ + L"\"..."; break;
		case LoadState::Error: cmp = L"Could not read \"" + compareName_ + L"\""; break;
		case LoadState::Ready:
			cmp = std::to_wstring(differenceCount()) + L" differ from \"" + compareName_ + L"\"";
			if (sourceOnly_) cmp += L", +" + std::to_wstring(sourceOnly_) + L" only there";
			break;
		}
	}

	::SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(s.c_str()));
	::SendMessageW(status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(cmp.c_str()));
	::SendMessageW(status, SB_SETTEXTW, 2,
	               reinterpret_cast<LPARAM>(isElevated() ? L"Administrator" : L"NOT elevated"));
	::SendMessageW(status, SB_SETTEXTW, 3,
	               reinterpret_cast<LPARAM>(isModernStandby() ? L"Modern Standby" : L"Classic standby"));
	const std::wstring build = L"Build " + std::to_wstring(osBuildNumber());
	::SendMessageW(status, SB_SETTEXTW, 4, reinterpret_cast<LPARAM>(build.c_str()));

	if (isModernStandby()) {
		::SetDlgItemTextW(hwnd_, ID_ADVISORY,
		                  L"This machine uses Modern Standby. Windows applies power-mode overlays "
		                  L"on top of Balanced; custom plans can conflict with the power slider.");
	}
}

void App::updatePendingBar() {
	const size_t n = pending_.count();
	std::wstring t = n == 1 ? L"1 pending change" : std::to_wstring(n) + L" pending changes";
	::SetDlgItemTextW(hwnd_, ID_PENDLABEL, t.c_str());
	::EnableWindow(::GetDlgItem(hwnd_, ID_APPLY), !applying_);
	::EnableWindow(::GetDlgItem(hwnd_, ID_DISCARD), !applying_);
	::SetDlgItemTextW(hwnd_, ID_APPLY, applying_ ? L"Applying..." : L"Apply");
	layout(); // show/hide the bar
}

void App::stageValue(const GUID& sub, const GUID& id, bool ac, DWORD value, bool validate) {
	const SettingEntry* e = findEntry(sub, id);
	if (!e) return;
	if (validate) {
		if (auto invalid = validateValue(e->desc, value)) {
			showInfo(L"Invalid value", *invalid);
			return;
		}
	}
	if (ac)
		pending_.stageAc(sub, id, e->values.ac, value);
	else
		pending_.stageDc(sub, id, e->values.dc, value);
	updatePendingBar();
	// Repaint the affected settings row (values/flags changed). Detail is not
	// rebuilt here so a trackbar drag / edit keeps focus.
	::InvalidateRect(::GetDlgItem(hwnd_, ID_SETTINGS), nullptr, FALSE);
}

// copying values across from the compare source

// One rail of one setting. Silent by design: a bulk copy must not put a message
// box in front of the user per skipped value.
//
// Deliberately does NOT run validateValue. Range/increment/enum metadata is read
// per setting, not per scheme (PowerReadValueMin takes no scheme argument), so
// both plans share one descriptor and there is no such thing as a value that is
// valid in the source but invalid here. The value being copied is already live
// in a real plan on this machine, which outranks our metadata: the increment is
// only a UI stepping hint and Windows stores unaligned values freely, and the
// possible-value list falls back to loop ordinals when the payload is not a
// DWORD. Pre-judging validity here rejected values that write perfectly well.
// The write itself is the authority; commit() reports what actually fails.
void App::stageFromSource(const SettingEntry& target, bool ac, CopySummary& sum) {
	const SettingEntry* src = sourceEntry(target.desc.subgroup, target.desc.id);
	if (!src) return; // handled once per setting by the caller
	const auto value = ac ? src->values.ac : src->values.dc;
	if (!value) {
		++sum.noValue;
		return;
	}
	// Policy locks are a real, enforced constraint, unlike the metadata above.
	if (ac ? target.desc.acLockedByPolicy : target.desc.dcLockedByPolicy) {
		sum.blocked.push_back(target.desc.name + L"  (" + (ac ? L"AC" : L"DC") +
		                      L")  -  locked by group policy");
		return;
	}
	if (const auto eff = effectiveValue(target, ac); eff && *eff == *value) {
		++sum.identical;
		return;
	}
	// stage() drops the entry when the new value equals the committed one, so
	// copying a value back to what is already on disk un-stages it.
	if (ac)
		pending_.stageAc(target.desc.subgroup, target.desc.id, target.values.ac, *value);
	else
		pending_.stageDc(target.desc.subgroup, target.desc.id, target.values.dc, *value);
	++sum.staged;
}

void App::copyFromSource(const std::vector<size_t>& indices, bool doAc, bool doDc, const wchar_t* scope) {
	if (!compareOn_ || compareState_ != LoadState::Ready || applying_) return;
	if (indices.empty()) return;

	const std::wstring who = compareName_.empty() ? L"the compare source" : compareName_;

	CopySummary sum;
	for (size_t i : indices) {
		if (i >= enumeration_.settings.size()) continue;
		const SettingEntry& e = enumeration_.settings[i];
		// Once per setting, not once per rail, so the report counts settings the
		// way the grid shows them.
		if (!sourceEntry(e.desc.subgroup, e.desc.id)) {
			sum.blocked.push_back(e.desc.name + L"  -  not present in \"" + who + L"\"");
			continue;
		}
		if (doAc) stageFromSource(e, true, sum);
		if (doDc) stageFromSource(e, false, sum);
	}

	updatePendingBar();
	rebuildDetail();
	updateStatus();
	// Deliberately not rebuildFilter(): that would collapse a multi-row selection
	// the user may still be working through. Copied rows read as "staged", so
	// they stay visible under "Differences only" until the next Apply anyway.
	::InvalidateRect(::GetDlgItem(hwnd_, ID_SETTINGS), nullptr, FALSE);

	// Report only what the user can act on. Values that already agreed, or that
	// the source has no value for, are the normal majority: counting them at each
	// other told the user "N already matched the source" next to a grid full of
	// differences, which reads as false even when the arithmetic is right.
	if (sum.blocked.empty() && sum.staged > 0) return; // pending bar is the feedback

	std::wstring msg;
	if (sum.staged) {
		msg = L"Staged " + std::to_wstring(sum.staged) + L" value(s) from \"" + who +
		      L"\".\n\nNothing is written until you choose Apply.";
	} else if (sum.blocked.empty()) {
		msg = L"Nothing to copy: " + std::wstring(scope) + L" already match \"" + who + L"\".";
	} else {
		msg = L"Nothing was copied.";
	}

	if (!sum.blocked.empty()) {
		// Named, so it can be checked against the grid rather than believed.
		msg += L"\n\nCould not copy " + std::to_wstring(sum.blocked.size()) + L":";
		const size_t show = std::min<size_t>(sum.blocked.size(), 12);
		for (size_t i = 0; i < show; ++i)
			msg += L"\n    " + sum.blocked[i];
		if (sum.blocked.size() > show)
			msg += L"\n    ... and " + std::to_wstring(sum.blocked.size() - show) + L" more";
	}
	showInfo(L"Copy from compare source", msg);
}

// actions

void App::applyPending() {
	if (pending_.empty() || applying_) return;
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const GUID scheme = *g;
	const std::wstring name = schemeList_[static_cast<size_t>(selectedScheme_)].name;

	std::wstring msg = L"Write " + std::to_wstring(pending_.count()) + L" change(s) to \"" + name + L"\"?";
	if (::MessageBoxW(hwnd_, msg.c_str(), L"Apply changes", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;

	applying_ = true;
	updatePendingBar();
	const PendingChangeSet changes = pending_;
	// Writes are quick; do them inline (already elevated). Re-enumerate after.
	auto report = values_.commit(scheme, changes);
	applying_ = false;
	if (!report) {
		showError(L"Apply failed", report.error());
	} else if (!report->allSucceeded()) {
		// The write is what decides whether a value is acceptable, so when one is
		// refused, say which setting and what Windows actually said. Showing only
		// failures.front() hid the scale of a partial failure after a bulk copy.
		const auto& fs = report->failures;
		std::wstring body = std::to_wstring(report->appliedWrites) + L" value(s) written, " +
		                    std::to_wstring(fs.size()) + L" refused by Windows:\n";
		const size_t show = std::min<size_t>(fs.size(), 10);
		for (size_t i = 0; i < show; ++i) {
			const SettingEntry* e = findEntry(fs[i].change.subgroup, fs[i].change.setting);
			body += L"\n    " + (e ? e->desc.name : guidToString(fs[i].change.setting)) + L"  -  " +
			        fs[i].error.systemMessage();
		}
		if (fs.size() > show)
			body += L"\n    ... and " + std::to_wstring(fs.size() - show) + L" more";
		::MessageBoxW(hwnd_, body.c_str(), L"Some changes could not be written", MB_OK | MB_ICONWARNING);
	}
	pending_.clear();
	updatePendingBar();
	startEnumeration();
}

// Gate for anything that would silently drop staged edits. Returns true when it
// is safe to proceed.
bool App::confirmDiscardPending(const wchar_t* action) {
	if (pending_.empty() || applying_) return true;
	std::wstring name = selectedScheme_ >= 0 && selectedScheme_ < static_cast<int>(schemeList_.size())
	                        ? schemeList_[static_cast<size_t>(selectedScheme_)].name
	                        : L"this plan";
	std::wstring msg = std::to_wstring(pending_.count()) + L" change(s) to \"" + name +
	                   L"\" have not been applied.\n\nDiscard them and " + action + L"?";
	return ::MessageBoxW(hwnd_, msg.c_str(), L"Unapplied changes", MB_OKCANCEL | MB_ICONWARNING) == IDOK;
}

void App::discardPending() {
	if (pending_.empty()) return;
	std::wstring msg = L"Discard " + std::to_wstring(pending_.count()) +
	                   L" pending change(s)?\n\nNothing has been written to the system.";
	if (::MessageBoxW(hwnd_, msg.c_str(), L"Discard changes", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
	pending_.clear();
	updatePendingBar();
	rebuildDetail();
	::InvalidateRect(::GetDlgItem(hwnd_, ID_SETTINGS), nullptr, FALSE);
}

void App::showPendingDiff() {
	if (pending_.empty()) return;
	std::wstring text;
	for (const auto& c : pending_.changes()) {
		const SettingEntry* e = findEntry(c.subgroup, c.setting);
		const std::wstring name = e ? e->desc.name : guidToString(c.setting);
		text += name + L"\r\n";
		if (c.acNew)
			text += L"    AC  " + indexText(c.acOld) + L"  ->  " + std::to_wstring(*c.acNew) + L"\r\n";
		if (c.dcNew)
			text += L"    DC  " + indexText(c.dcOld) + L"  ->  " + std::to_wstring(*c.dcNew) + L"\r\n";
	}
	::MessageBoxW(hwnd_, text.c_str(), L"Pending changes (nothing written until Apply)", MB_OK);
}

void App::showCreator() {
	CreateDlgData data;
	for (const auto& s : schemeList_)
		data.baseNames.push_back(s.name);
	data.baseSel = selectedScheme_ >= 0 ? static_cast<size_t>(selectedScheme_) : 0;
	data.guid = guidToString(generateGuid());
	if (::DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_CREATE), hwnd_, createDlgProc,
	                      reinterpret_cast<LPARAM>(&data)) != IDOK ||
	    !data.ok)
		return;

	const GUID dest = *guidFromString(data.guid);
	const GUID base = schemeList_[std::min(data.baseSel, schemeList_.size() - 1)].id;
	auto outcome = schemes_.duplicateScheme(base, dest);
	if (!outcome) {
		showError(L"Could not create the plan", outcome.error());
		return;
	}
	if (auto r = schemes_.setFriendlyName(dest, data.name); !r)
		showError(L"Plan created, but naming failed", r.error());
	if (!data.desc.empty()) schemes_.setDescription(dest, data.desc);

	loadSchemes(dest);
	showInfo(outcome->alreadyExisted ? L"Plan updated" : L"Plan created",
	         data.name + L"\n" + guidToString(dest));
}

void App::activateSelected() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	if (auto r = schemes_.setActiveScheme(*g); !r)
		showError(L"Could not activate the scheme", r.error());
	else
		loadSchemes();
}

void App::renameSelected() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const GUID scheme = *g;
	PromptDlgData d;
	d.title = L"Rename scheme";
	d.label = L"Name:";
	d.value = schemeList_[static_cast<size_t>(selectedScheme_)].name;
	if (::DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_PROMPT), hwnd_, promptDlgProc,
	                      reinterpret_cast<LPARAM>(&d)) != IDOK ||
	    !d.ok || d.value.empty())
		return;
	if (auto r = schemes_.setFriendlyName(scheme, d.value); !r)
		showError(L"Rename failed", r.error());
	else
		loadSchemes(scheme); // keep the renamed plan selected
}

void App::editDescriptionSelected() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const GUID scheme = *g;
	PromptDlgData d;
	d.title = L"Edit description";
	d.label = L"Description:";
	d.value = schemeList_[static_cast<size_t>(selectedScheme_)].description;
	if (::DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_PROMPT), hwnd_, promptDlgProc,
	                      reinterpret_cast<LPARAM>(&d)) != IDOK ||
	    !d.ok)
		return;
	if (auto r = schemes_.setDescription(scheme, d.value); !r)
		showError(L"Could not set description", r.error());
	else
		loadSchemes(scheme); // keep the edited plan selected
}

void App::duplicateSelected() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const GUID base = *g;
	const std::wstring baseName = schemeList_[static_cast<size_t>(selectedScheme_)].name;
	const GUID dest = generateGuid();
	auto outcome = schemes_.duplicateScheme(base, dest);
	if (!outcome) {
		showError(L"Could not duplicate the scheme", outcome.error());
		return;
	}
	schemes_.setFriendlyName(dest, baseName + L" (copy)");
	loadSchemes(dest);
}

void App::deleteSelected() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const SchemeInfo& s = schemeList_[static_cast<size_t>(selectedScheme_)];
	if (s.isActive) {
		showInfo(L"Cannot delete the active scheme", L"Activate another scheme first.");
		return;
	}
	const GUID scheme = s.id;
	std::wstring msg = L"Permanently delete \"" + s.name + L"\"?\n" + guidToString(scheme);
	if (::MessageBoxW(hwnd_, msg.c_str(), L"Delete scheme", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
	if (auto r = schemes_.deleteScheme(scheme); !r) {
		showError(L"Delete failed", r.error());
		return;
	}
	selectedScheme_ = -1;
	loadSchemes();
}

void App::browseImport() {
	if (auto path = openFileDialog(hwnd_, L"Power schemes (*.pow)\0*.pow\0All files (*.*)\0*.*\0",
	                               L"Import a .pow file", L""))
		importPow(*path);
}

void App::importPow(const std::wstring& path) {
	PowImporter importer;
	auto outcome = importer.importPow(path, std::nullopt);
	if (!outcome) {
		showError(L"Import failed", outcome.error());
		return;
	}
	const GUID imported = outcome->scheme;
	std::wstring name;
	if (auto n = schemes_.friendlyName(imported)) name = *n;
	size_t count = 0;
	if (auto e = enumerator_.enumerateScheme(imported)) count = e->settings.size();

	// Select the imported plan (does NOT change the active plan). It is now
	// fully editable, comparable, and exportable without activation.
	loadSchemes(imported);
	showInfo(L"Imported", L"Imported \"" + name + L"\"  (" + std::to_wstring(count) +
	                          L" settings).\n\nIt is selected and ready to edit, compare, or export. Your "
	                          L"active power plan was not changed. Use the Activate button only if you want "
	                          L"this plan to take effect on this machine.");
}

void App::exportSelected() {
	const GUID* g = selectedSchemeGuid();
	if (!g) return;
	const GUID scheme = *g;
	std::wstring defName = schemeList_[static_cast<size_t>(selectedScheme_)].name + L".pow";
	for (auto& ch : defName)
		if (wcschr(L"\\/:*?\"<>|", ch)) ch = L'_';
	auto path = saveFileDialog(hwnd_, L"Power schemes (*.pow)\0*.pow\0", L"pow", defName, L"Export .pow");
	if (!path) return;
	if (auto r = exportPow(scheme, *path); !r)
		showError(L"Export failed", r.error());
	else
		showInfo(L"Exported", *path);
}

// diff two plans

void App::showDiff() {
	if (schemeList_.size() < 1) {
		showInfo(L"Compare plans", L"No power plans are available to compare.");
		return;
	}
	diffRows_.clear();
	diffMergeA_ = diffMergeB_ = -1;
	const INT_PTR rc = ::DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_DIFF), hwnd_, diffDlgProc,
	                                     reinterpret_cast<LPARAM>(this));
	diffRows_.clear();
	if (rc != IDC_DIFF_MERGE || diffMergeA_ < 0 || diffMergeB_ < 0) return;

	// Hand the pair off to the main window, where the values can actually be
	// copied across: A becomes the plan under edit, B the compare source.
	const size_t a = static_cast<size_t>(diffMergeA_), b = static_cast<size_t>(diffMergeB_);
	if (a >= schemeList_.size() || b >= schemeList_.size() || a == b) return;
	const GUID target = schemeList_[a].id, source = schemeList_[b].id;

	if (!guidEqual(target, schemeList_[static_cast<size_t>(std::max(selectedScheme_, 0))].id)) {
		if (!confirmDiscardPending(L"compare a different plan")) return;
		pending_.clear();
		selectedScheme_ = static_cast<int>(a);
		updatePendingBar();
		refreshSchemeList();
		startEnumeration();
	}
	setCompareSource(source);
}

INT_PTR CALLBACK App::diffDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
	App* app = reinterpret_cast<App*>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
	switch (msg) {
	case WM_INITDIALOG: {
		app = reinterpret_cast<App*>(lParam);
		::SetWindowLongPtrW(dlg, GWLP_USERDATA, lParam);
		HWND ca = ::GetDlgItem(dlg, IDC_DIFF_A);
		HWND cb = ::GetDlgItem(dlg, IDC_DIFF_B);
		for (const auto& s : app->schemeList_) {
			::SendMessageW(ca, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.name.c_str()));
			::SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.name.c_str()));
		}
		const int aSel = app->selectedScheme_ >= 0 ? app->selectedScheme_ : 0;
		const int bSel = app->schemeList_.size() > 1 ? (aSel == 0 ? 1 : 0) : aSel;
		::SendMessageW(ca, CB_SETCURSEL, aSel, 0);
		::SendMessageW(cb, CB_SETCURSEL, bSel, 0);

		HWND lv = ::GetDlgItem(dlg, IDC_DIFF_LIST);
		ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
		struct {
			const wchar_t* t;
			int w;
		} cols[] = {{L"Setting", 150}, {L"Subgroup", 110}, {L"AC (A)", 60},
		            {L"AC (B)", 60},   {L"DC (A)", 60},    {L"DC (B)", 60}};
		for (int i = 0; i < 6; ++i) {
			LVCOLUMNW c{};
			c.mask = LVCF_TEXT | LVCF_WIDTH;
			c.pszText = const_cast<wchar_t*>(cols[i].t);
			c.cx = app->S(cols[i].w);
			ListView_InsertColumn(lv, i, &c);
		}
		return TRUE;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_DIFF_COMPARE && app) {
			app->runDiff(dlg);
			return TRUE;
		}
		if (LOWORD(wParam) == IDC_DIFF_MERGE && app) {
			const int aSel = static_cast<int>(::SendDlgItemMessageW(dlg, IDC_DIFF_A, CB_GETCURSEL, 0, 0));
			const int bSel = static_cast<int>(::SendDlgItemMessageW(dlg, IDC_DIFF_B, CB_GETCURSEL, 0, 0));
			if (aSel < 0 || bSel < 0 || aSel == bSel) {
				::MessageBoxW(dlg, L"Pick two different plans to merge between.", L"Compare power plans",
				              MB_ICONWARNING);
				return TRUE;
			}
			app->diffMergeA_ = aSel;
			app->diffMergeB_ = bSel;
			::EndDialog(dlg, IDC_DIFF_MERGE);
			return TRUE;
		}
		if (LOWORD(wParam) == IDCANCEL) {
			::EndDialog(dlg, 0);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

void App::runDiff(HWND dlg) {
	const int aSel = static_cast<int>(::SendDlgItemMessageW(dlg, IDC_DIFF_A, CB_GETCURSEL, 0, 0));
	const int bSel = static_cast<int>(::SendDlgItemMessageW(dlg, IDC_DIFF_B, CB_GETCURSEL, 0, 0));
	if (aSel < 0 || bSel < 0 || aSel >= static_cast<int>(schemeList_.size()) ||
	    bSel >= static_cast<int>(schemeList_.size()))
		return;

	HCURSOR prev = ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
	auto a = enumerator_.enumerateScheme(schemeList_[static_cast<size_t>(aSel)].id);
	auto b = enumerator_.enumerateScheme(schemeList_[static_cast<size_t>(bSel)].id);
	::SetCursor(prev);
	if (!a) {
		showError(L"Could not read plan A", a.error());
		return;
	}
	if (!b) {
		showError(L"Could not read plan B", b.error());
		return;
	}

	diffRows_ = computeDiff(*a, *b);
	HWND lv = ::GetDlgItem(dlg, IDC_DIFF_LIST);
	ListView_DeleteAllItems(lv);
	for (size_t i = 0; i < diffRows_.size(); ++i) {
		const DiffRow& r = diffRows_[i];
		LVITEMW it{};
		it.mask = LVIF_TEXT;
		it.iItem = static_cast<int>(i);
		it.pszText = const_cast<wchar_t*>(r.name.c_str());
		ListView_InsertItem(lv, &it);
		ListView_SetItemText(lv, static_cast<int>(i), 1, const_cast<wchar_t*>(r.subgroup.c_str()));
		ListView_SetItemText(lv, static_cast<int>(i), 2, const_cast<wchar_t*>(r.acA.c_str()));
		ListView_SetItemText(lv, static_cast<int>(i), 3, const_cast<wchar_t*>(r.acB.c_str()));
		ListView_SetItemText(lv, static_cast<int>(i), 4, const_cast<wchar_t*>(r.dcA.c_str()));
		ListView_SetItemText(lv, static_cast<int>(i), 5, const_cast<wchar_t*>(r.dcB.c_str()));
	}
	const std::wstring aName = schemeList_[static_cast<size_t>(aSel)].name;
	const std::wstring bName = schemeList_[static_cast<size_t>(bSel)].name;
	std::wstring summary =
	    diffRows_.empty()
	        ? L"No differences. \"" + aName + L"\" and \"" + bName + L"\" have identical AC/DC values."
	        : std::to_wstring(diffRows_.size()) + L" difference(s) between \"" + aName + L"\" (A) and \"" +
	              bName + L"\" (B).";
	::SetDlgItemTextW(dlg, IDC_DIFF_SUMMARY, summary.c_str());
}

// helpers

void App::showError(const std::wstring& title, const PowerError& e) {
	std::wstring body = e.operation + L"\nWin32 " + std::to_wstring(e.code) + L": " + e.systemMessage();
	if (e.isPolicyDisabled())
		body += L"\n\nThis action is locked by group policy.";
	else if (e.isAccessDenied())
		body += L"\n\nThe app must run as administrator.";
	if (!e.context.empty()) body += L"\n" + e.context;
	::MessageBoxW(hwnd_, body.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

void App::showInfo(const std::wstring& title, const std::wstring& msg) {
	::MessageBoxW(hwnd_, msg.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

void App::showAbout() {
	std::wstring text = L"fspwr\r\n\r\nWindows power scheme inspector and editor.\r\n\r\n";
	text += isElevated() ? L"Running as administrator.\r\n" : L"Not running as administrator.\r\n";
	text += L"OS build " + std::to_wstring(osBuildNumber());
	text += isModernStandby() ? L", Modern Standby." : L", classic standby.";
	::MessageBoxW(hwnd_, text.c_str(), L"About fspwr", MB_OK | MB_ICONINFORMATION);
}

void App::copyToClipboard(const std::wstring& text) {
	if (!::OpenClipboard(hwnd_)) return;
	::EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
		if (void* p = ::GlobalLock(mem)) {
			::memcpy(p, text.c_str(), bytes);
			::GlobalUnlock(mem);
			::SetClipboardData(CF_UNICODETEXT, mem);
		} else {
			::GlobalFree(mem);
		}
	}
	::CloseClipboard();
}

// window proc

LRESULT App::wndProc(UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_CREATE: return 0;

	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) layout();
		return 0;

	case WM_GETMINMAXINFO: {
		auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
		mmi->ptMinTrackSize.x = S(1024);
		mmi->ptMinTrackSize.y = S(700);
		return 0;
	}

	case WM_DPICHANGED: {
		dpi_ = HIWORD(wParam);
		applyFonts();
		for (HWND c = ::GetWindow(hwnd_, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT))
			::SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
		const RECT* r = reinterpret_cast<RECT*>(lParam);
		::SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
		               SWP_NOZORDER | SWP_NOACTIVATE);
		rebuildDetail();
		return 0;
	}

	case WM_INITMENUPOPUP: updateMenuState(); return 0;

	case WM_COMMAND: {
		const int id = LOWORD(wParam);
		const int code = HIWORD(wParam);
		HWND ctrl = reinterpret_cast<HWND>(lParam);
		switch (id) {
		case ID_CREATE: showCreator(); return 0;
		case ID_IMPORT: browseImport(); return 0;
		case ID_DIFF: showDiff(); return 0;
		case ID_ABOUT: showAbout(); return 0;
		case ID_EXIT: ::PostMessageW(hwnd_, WM_CLOSE, 0, 0); return 0;
		case ID_REFRESH:
			if (!applying_) {
				startEnumeration();
				if (compareOn_) {
					++compareGeneration_;
					compareState_ = LoadState::Loading;
					startCompareEnumeration();
				}
			}
			return 0;
		case ID_COMPARE:
			if (code == CBN_SELCHANGE) {
				const int sel = static_cast<int>(::SendMessageW(ctrl, CB_GETCURSEL, 0, 0));
				if (sel <= 0 || static_cast<size_t>(sel - 1) >= compareCombo_.size()) {
					setCompareSource(std::nullopt);
				} else {
					setCompareSource(schemeList_[compareCombo_[static_cast<size_t>(sel - 1)]].id);
				}
			}
			return 0;
		case ID_DIFFONLY:
			if (code == BN_CLICKED) {
				filterDiffOnly_ = ::SendMessageW(ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
				rebuildFilter();
			}
			return 0;
		case ID_COPYSEL:
		case ID_CTX_COPY:
			copyFromSource(selectedSettingIndices(), true, true, L"the selected setting(s)");
			return 0;
		case ID_CTX_COPY_AC:
			copyFromSource(selectedSettingIndices(), true, false, L"the selected setting(s), AC only");
			return 0;
		case ID_CTX_COPY_DC:
			copyFromSource(selectedSettingIndices(), false, true, L"the selected setting(s), DC only");
			return 0;
		case ID_COPYALL:
			// By value: the summary dialog pumps messages, and a re-enumeration
			// arriving mid-dialog would rebuild filtered_ underneath us.
			copyFromSource(std::vector<size_t>(filtered_), true, true, L"all shown settings");
			return 0;
		case ID_CTX_REVERT: {
			size_t n = 0;
			for (size_t i : selectedSettingIndices()) {
				if (i >= enumeration_.settings.size()) continue;
				const SettingDescriptor& d = enumeration_.settings[i].desc;
				if (pending_.find(d.subgroup, d.id)) {
					pending_.unstage(d.subgroup, d.id);
					++n;
				}
			}
			if (n) {
				updatePendingBar();
				rebuildDetail();
				updateStatus(); // the difference count moved
				::InvalidateRect(::GetDlgItem(hwnd_, ID_SETTINGS), nullptr, FALSE);
			}
			return 0;
		}
		case ID_ACTIVATE: activateSelected(); return 0;
		case ID_RENAME: renameSelected(); return 0;
		case ID_DESC: editDescriptionSelected(); return 0;
		case ID_DUP: duplicateSelected(); return 0;
		case ID_EXPORT: exportSelected(); return 0;
		case ID_DELETE: deleteSelected(); return 0;
		case ID_APPLY: applyPending(); return 0;
		case ID_DISCARD: discardPending(); return 0;
		case ID_VIEWDIFF: showPendingDiff(); return 0;
		case ID_SEARCH:
			if (code == EN_CHANGE) {
				wchar_t buf[256];
				::GetWindowTextW(ctrl, buf, 256);
				filterText_ = buf;
				rebuildFilter();
			}
			return 0;
		case ID_SUBGROUP:
			if (code == CBN_SELCHANGE) {
				selectedSubgroup_ = static_cast<int>(::SendMessageW(ctrl, CB_GETCURSEL, 0, 0));
				rebuildFilter();
			}
			return 0;
		case ID_HIDDEN:
			if (code == BN_CLICKED) {
				filterHidden_ = ::SendMessageW(ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
				rebuildFilter();
			}
			return 0;
		case ID_MODIFIED:
			if (code == BN_CLICKED) {
				filterModified_ = ::SendMessageW(ctrl, BM_GETCHECK, 0, 0) == BST_CHECKED;
				rebuildFilter();
			}
			return 0;
		default:
			// Dynamic detail editors (combos, edits, reset/guid buttons).
			if (ctrl && editors_.count(ctrl)) {
				onEditorCommand(ctrl, code);
				return 0;
			}
			break;
		}
		return 0;
	}

	case WM_HSCROLL:
		if (HWND bar = reinterpret_cast<HWND>(lParam); bar && editors_.count(bar)) onTrackbar(bar);
		return 0;

	case WM_CONTEXTMENU: {
		HWND from = reinterpret_cast<HWND>(wParam);
		const bool onSchemes = from == ::GetDlgItem(hwnd_, ID_SCHEMES);
		const bool onSettings = from == ::GetDlgItem(hwnd_, ID_SETTINGS);
		if (onSchemes || onSettings) {
			int x = GET_X_LPARAM(lParam), yy = GET_Y_LPARAM(lParam);
			if (x == -1 && yy == -1) { // keyboard (Shift+F10 / menu key)
				RECT r{};
				::GetWindowRect(from, &r);
				x = r.left + S(16);
				yy = r.top + S(16);
			}
			if (onSchemes)
				showSchemeMenu(x, yy);
			else
				showSettingMenu(x, yy);
			return 0;
		}
		break;
	}

	case WM_NOTIFY: {
		auto* hdr = reinterpret_cast<NMHDR*>(lParam);
		if (hdr->idFrom == ID_SETTINGS) {
			if (hdr->code == LVN_GETDISPINFOW) {
				auto* di = reinterpret_cast<NMLVDISPINFOW*>(lParam);
				const SettingEntry* e = rowEntry(static_cast<size_t>(di->item.iItem));
				if (e && (di->item.mask & LVIF_TEXT)) {
					static thread_local std::wstring buf;
					// The column set differs between plain and compare mode; use the
					// one the list actually has, not the one state says it should.
					auto sourceText = [&](bool ac) -> std::wstring {
						if (compareState_ != LoadState::Ready) return L"...";
						const SettingEntry* s = sourceEntry(e->desc.subgroup, e->desc.id);
						if (!s) return L"(absent)";
						return e->desc.renderValue(ac ? s->values.ac : s->values.dc);
					};
					if (compareColumns_) {
						switch (di->item.iSubItem) {
						// Blank rather than a misleading glyph until the source is read.
						case 0:
							buf = compareState_ == LoadState::Ready ? diffGlyph(diffStateOf(*e)) : L"";
							break;
						case 1: buf = e->desc.name; break;
						case 2: buf = subgroupName(e->desc.subgroup); break;
						case 3: buf = e->desc.renderValue(effectiveValue(*e, true)); break;
						case 4: buf = sourceText(true); break;
						case 5: buf = e->desc.renderValue(effectiveValue(*e, false)); break;
						case 6: buf = sourceText(false); break;
						default: buf.clear();
						}
					} else {
						switch (di->item.iSubItem) {
						case 0: buf = e->desc.name; break;
						case 1: buf = subgroupName(e->desc.subgroup); break;
						case 2: buf = e->desc.renderValue(effectiveValue(*e, true)); break;
						case 3: buf = e->desc.renderValue(effectiveValue(*e, false)); break;
						case 4: {
							buf.clear();
							if (e->desc.hidden) buf += L"Hidden ";
							if (e->desc.acLockedByPolicy || e->desc.dcLockedByPolicy) buf += L"Policy ";
							if (isModified(*e)) buf += L"Modified ";
							if (e->desc.metadataIncomplete) buf += L"Raw ";
							break;
						}
						default: buf.clear();
						}
					}
					di->item.pszText = buf.data();
				}
				return 0;
			}
			if (hdr->code == LVN_ITEMCHANGED) {
				auto* nm = reinterpret_cast<NMLISTVIEW*>(lParam);
				// Track focus, not selection: extending a multi-row selection
				// should not rebuild the editors on every row it sweeps over.
				if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_FOCUSED) &&
				    (nm->uNewState & LVIS_FOCUSED)) {
					rebuildDetail();
				}
				return 0;
			}
		} else if (hdr->idFrom == ID_SCHEMES) {
			if (hdr->code == LVN_ITEMCHANGED && !suppressSchemeSelect_) {
				auto* nm = reinterpret_cast<NMLISTVIEW*>(lParam);
				if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) &&
				    nm->iItem != selectedScheme_) {
					// Switching plans drops the staged edits. That is cheap to do by
					// accident and expensive to redo once a copy has staged dozens of
					// settings, so confirm first -- but not from inside this
					// notification, which runs while the list holds mouse capture.
					// Post it and let the click finish.
					::PostMessageW(hwnd_, WM_APP_PLAN_SWITCH, static_cast<WPARAM>(nm->iItem), 0);
				}
				return 0;
			}
		}
		break;
	}

	case WM_APP_ENUM_DONE: onEnumDone(reinterpret_cast<EnumResult*>(lParam)); return 0;

	case WM_APP_CMP_DONE: onCompareDone(reinterpret_cast<EnumResult*>(lParam)); return 0;

	case WM_APP_PLAN_SWITCH: {
		const int target = static_cast<int>(wParam);
		if (target < 0 || target >= static_cast<int>(schemeList_.size()) || target == selectedScheme_)
			return 0;
		if (!confirmDiscardPending(L"switch to another plan")) {
			// Put the highlight back on the plan that still owns the staged edits.
			suppressSchemeSelect_ = true;
			HWND lv = ::GetDlgItem(hwnd_, ID_SCHEMES);
			ListView_SetItemState(lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
			if (selectedScheme_ >= 0) {
				ListView_SetItemState(lv, selectedScheme_, LVIS_SELECTED | LVIS_FOCUSED,
				                      LVIS_SELECTED | LVIS_FOCUSED);
			}
			suppressSchemeSelect_ = false;
			return 0;
		}
		selectedScheme_ = target;
		pending_.clear();
		updatePendingBar();
		// The compare source must not also be the plan being edited.
		if (compareOn_ && guidEqual(schemeList_[static_cast<size_t>(target)].id, compareSource_)) {
			setCompareSource(std::nullopt);
		} else {
			rebuildCompareCombo();
		}
		startEnumeration();
		return 0;
	}

	case WM_DROPFILES: {
		HDROP drop = reinterpret_cast<HDROP>(wParam);
		wchar_t path[MAX_PATH]{};
		if (::DragQueryFileW(drop, 0, path, MAX_PATH) > 0) importPow(path);
		::DragFinish(drop);
		return 0;
	}

	case WM_ACTIVATE:
		if (LOWORD(wParam) != WA_INACTIVE && enumState_ != LoadState::Loading && !applying_ &&
		    !enumeration_.settings.empty())
			startEnumeration(); // pick up external changes on focus
		return 0;

	case WM_CLOSE: ::DestroyWindow(hwnd_); return 0;

	case WM_DESTROY: ::PostQuitMessage(0); return 0;
	}
	return ::DefWindowProcW(hwnd_, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
	HANDLE mutex = ::CreateMutexW(nullptr, TRUE, L"fspwr.SingleInstance");
	if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
		if (HWND existing = ::FindWindowW(L"fspwrWindow", nullptr)) ::SetForegroundWindow(existing);
		return 0;
	}
	const HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	App app;
	const int rc = app.run(instance, showCmd);
	if (SUCCEEDED(co)) ::CoUninitialize();
	if (mutex) ::CloseHandle(mutex);
	return rc;
}
