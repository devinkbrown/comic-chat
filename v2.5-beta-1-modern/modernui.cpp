#if defined(_WIN32)
#include "stdafx.h"
#endif

#include "modernui.h"

#include <algorithm>
#include <iterator>

#if defined(_WIN32)
#include <map>
#include <mutex>
#endif

namespace comic_chat::modern_ui {

Metrics MetricsForDpi(std::uint32_t dpi)
{
	if (dpi < 48 || dpi > 768) dpi = 96;
	int icon = 20;
	if (dpi > 96) icon = 24;
	if (dpi > 120) icon = 32;
	if (dpi > 168) icon = 40;
	if (dpi > 216) icon = 48;
	return {
		dpi,
		Scale(32, dpi),
		icon,
		Scale(38, dpi),
		Scale(32, dpi),
		Scale(12, dpi),
		Scale(24, dpi),
		(std::max)(1, Scale(1, dpi)),
	};
}

Palette PaletteFor(bool dark, bool high_contrast, const SystemColors& system)
{
	if (high_contrast) {
		return {
			system.window,
			system.window_text,
			system.button_face,
			system.highlight,
			system.highlight,
			system.highlight,
			system.highlight,
			system.gray_text,
			true,
			dark,
		};
	}
	if (dark) {
		return {
			{32, 34, 37},
			{247, 244, 232},
			{24, 26, 29},
			{93, 159, 216},
			{246, 196, 69},
			{73, 184, 143},
			{222, 101, 101},
			{165, 169, 174},
			false,
			true,
		};
	}
	return {
		{247, 244, 232},
		{23, 25, 28},
		{235, 232, 222},
		{40, 103, 165},
		{246, 196, 69},
		{22, 122, 91},
		{180, 58, 58},
		{105, 108, 112},
		false,
		false,
	};
}

ConnectionLabels LabelsFor(const ConnectionState& state)
{
	ConnectionLabels labels;
	switch (state.transport) {
	case TransportState::offline: labels.transport = "Offline"; break;
	case TransportState::connecting: labels.transport = "Connecting"; break;
	case TransportState::reconnecting: labels.transport = "Reconnecting"; break;
	case TransportState::online: labels.transport = "Online"; break;
	}
	if (state.transport == TransportState::offline) {
		labels.security = "No connection";
		labels.authentication = "Signed out";
	} else if (state.transport != TransportState::online) {
		// Never surface security/authentication from a previous transport
		// generation while the replacement generation is still negotiating.
		labels.security = "Security pending";
		labels.authentication = "Signing in";
	} else {
		labels.security = state.secure ? "TLS verified" : "Plaintext";
		if (!state.registration_finished) labels.authentication = "Signing in";
		else if (state.sasl_succeeded) labels.authentication = "SASL account";
		else labels.authentication = "Anonymous";
	}
	return labels;
}

bool IconSizeIsSupported(int size)
{
	return size == 16 || size == 20 || size == 24 || size == 32 || size == 40 || size == 48;
}

#if defined(_WIN32)
namespace {

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
using SetProcessDpiAwareFn = BOOL(WINAPI*)();
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
using GetDpiForSystemFn = UINT(WINAPI*)();
using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

thread_local HHOOK g_dialog_hook = nullptr;
thread_local unsigned int g_dialog_scope_count = 0;

struct FontCache {
	std::mutex mutex;
	std::map<std::uint32_t, HFONT> fonts;
	~FontCache()
	{
		for (const auto& [dpi, font] : fonts) ::DeleteObject(font);
	}
};

FontCache& Fonts()
{
	static FontCache cache;
	return cache;
}

Color SystemColor(int index)
{
	const COLORREF color = ::GetSysColor(index);
	return {
		static_cast<std::uint8_t>(GetRValue(color)),
		static_cast<std::uint8_t>(GetGValue(color)),
		static_cast<std::uint8_t>(GetBValue(color)),
	};
}

bool FontExists(const wchar_t* face)
{
	HDC dc = ::GetDC(nullptr);
	if (!dc) return false;
	LOGFONTW font{};
	font.lfCharSet = DEFAULT_CHARSET;
	wcsncpy_s(font.lfFaceName, face, _TRUNCATE);
	bool found = false;
	::EnumFontFamiliesExW(dc, &font,
		[](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM context) -> int {
			*reinterpret_cast<bool*>(context) = true;
			return 0;
		}, reinterpret_cast<LPARAM>(&found), 0);
	::ReleaseDC(nullptr, dc);
	return found;
}

bool IsThemeableControl(HWND window)
{
	wchar_t class_name[64]{};
	::GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
	return _wcsicmp(class_name, L"SysListView32") == 0 ||
		_wcsicmp(class_name, L"SysTreeView32") == 0 ||
		_wcsicmp(class_name, L"SysTabControl32") == 0 ||
		_wcsicmp(class_name, L"ToolbarWindow32") == 0 ||
		_wcsicmp(class_name, L"ReBarWindow32") == 0 ||
		_wcsicmp(class_name, L"msctls_statusbar32") == 0 ||
		_wcsicmp(class_name, L"Button") == 0 ||
		_wcsicmp(class_name, L"ComboBox") == 0 ||
		_wcsicmp(class_name, L"Edit") == 0;
}

void ThemeOneWindow(HWND window, bool root)
{
	if (!window) return;
	const bool high_contrast = HighContrastEnabled();
	const bool dark = !high_contrast && DarkModePreferred();
	if ((root || IsThemeableControl(window))) {
		if (HMODULE theme = ::LoadLibraryW(L"uxtheme.dll")) {
			if (const auto set_theme = reinterpret_cast<SetWindowThemeFn>(
				::GetProcAddress(theme, "SetWindowTheme"))) {
				set_theme(window, high_contrast ? L"" : (dark ? L"DarkMode_Explorer" : L"Explorer"), nullptr);
			}
			::FreeLibrary(theme);
		}
	}
	if (root) {
		if (HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll")) {
			if (const auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(
				::GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
				const BOOL enabled = dark ? TRUE : FALSE;
				if (FAILED(set_attribute(window, 20, &enabled, sizeof(enabled))))
					(void)set_attribute(window, 19, &enabled, sizeof(enabled));
			}
			::FreeLibrary(dwm);
		}
	}
}

BOOL CALLBACK ThemeChild(HWND child, LPARAM)
{
	// Never replace child fonts here. Comic, RichEdit, preview, and custom
	// controls own typography that conveys document content.
	ThemeOneWindow(child, false);
	return TRUE;
}

LRESULT CALLBACK DialogHook(int code, WPARAM wparam, LPARAM lparam)
{
	if (code == HCBT_ACTIVATE)
		ApplyWindowTheme(reinterpret_cast<HWND>(wparam), true);
	return ::CallNextHookEx(g_dialog_hook, code, wparam, lparam);
}

} // namespace

bool EnablePerMonitorV2DpiAwareness()
{
	if (HMODULE user = ::GetModuleHandleW(L"user32.dll")) {
		if (const auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
			::GetProcAddress(user, "SetProcessDpiAwarenessContext"))) {
			if (set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;
		}
		if (const auto set_aware = reinterpret_cast<SetProcessDpiAwareFn>(
			::GetProcAddress(user, "SetProcessDPIAware"))) return set_aware() != FALSE;
	}
	return false;
}

std::uint32_t SystemDpi()
{
	if (HMODULE user = ::GetModuleHandleW(L"user32.dll")) {
		if (const auto get_dpi = reinterpret_cast<GetDpiForSystemFn>(
			::GetProcAddress(user, "GetDpiForSystem"))) return get_dpi();
	}
	HDC dc = ::GetDC(nullptr);
	const auto dpi = dc ? static_cast<std::uint32_t>(::GetDeviceCaps(dc, LOGPIXELSX)) : 96U;
	if (dc) ::ReleaseDC(nullptr, dc);
	return dpi ? dpi : 96U;
}

std::uint32_t DpiForWindow(HWND window)
{
	if (HMODULE user = ::GetModuleHandleW(L"user32.dll")) {
		if (const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
			::GetProcAddress(user, "GetDpiForWindow"))) {
			const UINT dpi = get_dpi(window);
			if (dpi) return dpi;
		}
	}
	return SystemDpi();
}

bool HighContrastEnabled()
{
	HIGHCONTRASTW contrast{sizeof(contrast)};
	return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
		(contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool DarkModePreferred()
{
	DWORD light = 1;
	DWORD bytes = sizeof(light);
	if (::RegGetValueW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes) != ERROR_SUCCESS)
		return false;
	return light == 0;
}

Palette PaletteForWindow(HWND)
{
	return PaletteFor(DarkModePreferred(), HighContrastEnabled(), {
		SystemColor(COLOR_WINDOW),
		SystemColor(COLOR_WINDOWTEXT),
		SystemColor(COLOR_BTNFACE),
		SystemColor(COLOR_BTNTEXT),
		SystemColor(COLOR_HIGHLIGHT),
		SystemColor(COLOR_HIGHLIGHTTEXT),
		SystemColor(COLOR_GRAYTEXT),
	});
}

COLORREF ToColorRef(Color color)
{
	return RGB(color.red, color.green, color.blue);
}

HFONT UiFont(std::uint32_t dpi)
{
	if (!dpi) dpi = 96;
	auto& cache = Fonts();
	std::lock_guard lock(cache.mutex);
	if (const auto found = cache.fonts.find(dpi); found != cache.fonts.end()) return found->second;
	LOGFONTW font{};
	font.lfHeight = -Scale(12, dpi);
	font.lfWeight = FW_NORMAL;
	font.lfQuality = CLEARTYPE_QUALITY;
	wcscpy_s(font.lfFaceName,
		FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI");
	HFONT handle = ::CreateFontIndirectW(&font);
	cache.fonts[dpi] = handle;
	return handle;
}

void ApplyWindowTheme(HWND window, bool include_children)
{
	ThemeOneWindow(window, true);
	if (include_children) ::EnumChildWindows(window, ThemeChild, 0);
	::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

ScopedDialogTheme::ScopedDialogTheme()
{
	if (++g_dialog_scope_count == 1)
		g_dialog_hook = ::SetWindowsHookExW(WH_CBT, DialogHook, nullptr, ::GetCurrentThreadId());
}

ScopedDialogTheme::~ScopedDialogTheme()
{
	if (g_dialog_scope_count && --g_dialog_scope_count == 0 && g_dialog_hook) {
		::UnhookWindowsHookEx(g_dialog_hook);
		g_dialog_hook = nullptr;
	}
}

#endif

} // namespace comic_chat::modern_ui
