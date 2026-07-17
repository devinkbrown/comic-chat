#ifndef COMIC_CHAT_MODERN_UI_H
#define COMIC_CHAT_MODERN_UI_H

#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <commctrl.h>
#endif

namespace comic_chat::modern_ui {

struct Color {
	std::uint8_t red;
	std::uint8_t green;
	std::uint8_t blue;
	friend constexpr bool operator==(Color, Color) = default;
};

struct SystemColors {
	Color window;
	Color window_text;
	Color button_face;
	Color button_text;
	Color highlight;
	Color highlight_text;
	Color gray_text;
};

struct Palette {
	Color paper;
	Color ink;
	Color surface;
	Color caption;
	Color action;
	Color secure;
	Color alert;
	Color muted;
	bool high_contrast = false;
	bool dark = false;
};

struct Metrics {
	std::uint32_t dpi;
	int target;
	int icon;
	int toolbar_height;
	int tab_height;
	int tab_padding;
	int status_height;
	int border;
};

enum class TransportState { offline, connecting, reconnecting, online };

struct ConnectionState {
	TransportState transport = TransportState::offline;
	bool secure = false;
	bool registration_finished = false;
	bool sasl_succeeded = false;
};

struct ConnectionLabels {
	std::string transport;
	std::string security;
	std::string authentication;
};

constexpr int Scale(int value, std::uint32_t dpi)
{
	return static_cast<int>((static_cast<std::int64_t>(value) * (dpi ? dpi : 96) + 48) / 96);
}

Metrics MetricsForDpi(std::uint32_t dpi);
Palette PaletteFor(bool dark, bool high_contrast, const SystemColors& system);
ConnectionLabels LabelsFor(const ConnectionState& state);
bool IconSizeIsSupported(int size);

#if defined(_WIN32)

bool EnablePerMonitorV2DpiAwareness();
std::uint32_t SystemDpi();
std::uint32_t DpiForWindow(HWND window);
bool HighContrastEnabled();
bool DarkModePreferred();
Palette PaletteForWindow(HWND window);
COLORREF ToColorRef(Color color);
HFONT UiFont(std::uint32_t dpi);
void ApplyWindowTheme(HWND window, bool include_children = true);
class ScopedDialogTheme {
public:
	ScopedDialogTheme();
	~ScopedDialogTheme();
	ScopedDialogTheme(const ScopedDialogTheme&) = delete;
	ScopedDialogTheme& operator=(const ScopedDialogTheme&) = delete;
};

#endif

} // namespace comic_chat::modern_ui

#endif
