#include "../iconcatalog.h"
#include "../modernui.h"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string_view>

namespace {

int failures = 0;

void Check(bool condition, std::string_view description)
{
	if (condition) return;
	std::cerr << "FAIL: " << description << '\n';
	++failures;
}

template <std::size_t Size>
void CheckStrip(
	const std::array<comic_chat::modern_ui::IconBinding, Size>& bindings,
	std::string_view name,
	bool allow_zero = false)
{
	std::set<std::uint32_t> commands;
	std::set<std::string_view> semantics;
	for (const auto& binding : bindings) {
		Check(!binding.semantic_name.empty(), "icon semantic name is present");
		Check(semantics.insert(binding.semantic_name).second, "icon semantics are unique within strip");
		if (binding.command == 0 && allow_zero) continue;
		Check(binding.command != 0, "active icon has a command");
		Check(commands.insert(binding.command).second, "icon commands are unique within strip");
	}
	(void)name;
}

void TestMetrics()
{
	using namespace comic_chat::modern_ui;
	const auto at_96 = MetricsForDpi(96);
	const auto at_144 = MetricsForDpi(144);
	const auto at_192 = MetricsForDpi(192);
	Check(at_96.target >= 32 && at_96.tab_height >= 32, "96-DPI targets meet accessibility floor");
	Check(at_144.target == 48 && at_192.target == 64, "metrics scale linearly per monitor");
	Check(at_96.icon == 20 && at_144.icon == 32 && at_192.icon == 40,
		"icons select deterministic multi-DPI rasters");
	for (const int size : {16, 20, 24, 32, 40, 48}) Check(IconSizeIsSupported(size), "catalog icon size supported");
	Check(!IconSizeIsSupported(36), "uncatalogued icon size rejected");
}

void TestPalette()
{
	using namespace comic_chat::modern_ui;
	const SystemColors system{
		{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12},
		{13, 14, 15}, {16, 17, 18}, {19, 20, 21},
	};
	const auto light = PaletteFor(false, false, system);
	Check(light.paper == Color{247, 244, 232} && light.ink == Color{23, 25, 28},
		"inked-workspace light tokens are stable");
	const auto dark = PaletteFor(true, false, system);
	Check(dark.dark && dark.paper == Color{32, 34, 37}, "dark palette has intentional paper inversion");
	const auto contrast = PaletteFor(true, true, system);
	Check(contrast.high_contrast && contrast.paper == system.window &&
		contrast.ink == system.window_text && contrast.caption == system.highlight,
		"high contrast defers exclusively to system colors");
}

void TestConnectionLabels()
{
	using namespace comic_chat::modern_ui;
	Check(LabelsFor({TransportState::offline, false, false, false}).security == "No connection",
		"offline security label is explicit");
	const auto reconnecting = LabelsFor({TransportState::reconnecting, true, false, false});
	Check(reconnecting.transport == "Reconnecting" && reconnecting.security == "Security pending" &&
		reconnecting.authentication == "Signing in", "reconnect state cannot leak prior TLS/login claims");
	const auto connecting = LabelsFor({TransportState::connecting, true, true, true});
	Check(connecting.security == "Security pending" && connecting.authentication == "Signing in",
		"connecting generation never claims TLS or SASL success");
	const auto sasl = LabelsFor({TransportState::online, true, true, true});
	Check(sasl.transport == "Online" && sasl.authentication == "SASL account",
		"successful authenticated connection is unambiguous");
}

void TestIconCatalog()
{
	using namespace comic_chat::modern_ui;
	CheckStrip(kMainToolbarIcons, "main");
	CheckStrip(kTextToolbarIcons, "text");
	CheckStrip(kUserToolbarIcons, "member", true);
	CheckStrip(kRoomTabIcons, "tabs", true);
	Check(kMainToolbarIcons[0].command == ID_SESSION_CONNECT &&
		kMainToolbarIcons[1].command == ID_SESSION_DISCONNECT &&
		kMainToolbarIcons[9].command == ID_FAVORITES_OPENFAVORITES,
		"main strip preserves original command order");
	Check(kUserToolbarIcons[6].command == 0 &&
		kUserToolbarIcons[6].glyph == Glyph::netmeeting,
		"obsolete NetMeeting source cell is catalogued but not relabelled");
	for (const auto& strip : kIconStrips) {
		Check(FindIconStrip(strip.resource) == &strip, "resource resolves to exact icon strip");
		for (std::size_t index = 0; index < strip.binding_count; ++index) {
			const auto command = strip.bindings[index].command;
			if (command)
				Check(FindIconIndex(strip.resource, command) == static_cast<int>(index),
					"command resolves to exact source strip index");
		}
	}
}

} // namespace

int main()
{
	TestMetrics();
	TestPalette();
	TestConnectionLabels();
	TestIconCatalog();
	if (failures) return EXIT_FAILURE;
	std::cout << "Modern UI tests passed\n";
	return EXIT_SUCCESS;
}
