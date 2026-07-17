#ifndef COMIC_CHAT_ICON_CATALOG_H
#define COMIC_CHAT_ICON_CATALOG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "resource.h"

namespace comic_chat::modern_ui {

enum class Glyph : std::uint8_t {
	connect,
	disconnect,
	new_room,
	leave_room,
	create_room,
	comic_view,
	text_view,
	room_list,
	user_list,
	favorites,
	font,
	color,
	bold,
	italic,
	underline,
	fixed_pitch,
	symbol,
	away,
	identity,
	ignore,
	whisper,
	email,
	homepage,
	netmeeting,
	tab_room,
	tab_new_content,
	tab_status,
	tab_alert,
};

struct IconBinding {
	std::uint32_t command;
	Glyph glyph;
	std::string_view semantic_name;
};

struct IconStrip {
	std::uint32_t resource;
	std::string_view source_bitmap;
	const IconBinding* bindings;
	std::size_t binding_count;
};

inline constexpr std::array<IconBinding, 10> kMainToolbarIcons{{
	{ID_SESSION_CONNECT, Glyph::connect, "connect"},
	{ID_SESSION_DISCONNECT, Glyph::disconnect, "disconnect"},
	{ID_SESSION_NEWROOM, Glyph::new_room, "new-room"},
	{ID_SESSION_LEAVE, Glyph::leave_room, "leave-room"},
	{ID_ROOM_CREATEROOM, Glyph::create_room, "create-room"},
	{ID_VIEW_COMICS, Glyph::comic_view, "comic-view"},
	{ID_VIEW_TEXT, Glyph::text_view, "text-view"},
	{ID_CHATROOM_LIST, Glyph::room_list, "room-list"},
	{ID_USER_LIST, Glyph::user_list, "user-list"},
	{ID_FAVORITES_OPENFAVORITES, Glyph::favorites, "favorites"},
}};

inline constexpr std::array<IconBinding, 7> kTextToolbarIcons{{
	{ID_SETFONT, Glyph::font, "font"},
	{ID_SETCOLOR, Glyph::color, "color"},
	{ID_SWITCHBOLD, Glyph::bold, "bold"},
	{ID_SWITCHITALIC, Glyph::italic, "italic"},
	{ID_SWITCHUNDERLINED, Glyph::underline, "underline"},
	{ID_SWITCHFIXEDPITCH, Glyph::fixed_pitch, "fixed-pitch"},
	{ID_SWITCHSYMBOL, Glyph::symbol, "symbol"},
}};

inline constexpr std::array<IconBinding, 7> kUserToolbarIcons{{
	{ID_AWAY_TOGGLE, Glyph::away, "away"},
	{ID_GETIDENTITY, Glyph::identity, "identity"},
	{ID_MEMBER_IGNORE, Glyph::ignore, "ignore"},
	{ID_WHISPERBOX_MLIST, Glyph::whisper, "whisper"},
	{ID_SEND_EMAIL, Glyph::email, "email"},
	{ID_VISIT_HOMEPAGE, Glyph::homepage, "homepage"},
	// Microsoft authored this physical cell for NetMeeting.  The obsolete
	// command is omitted from the toolbar, but the catalog retains the source
	// cell and never misrepresents its glyph as another action.
	{0, Glyph::netmeeting, "netmeeting-obsolete"},
}};

inline constexpr std::array<IconBinding, 4> kRoomTabIcons{{
	{0, Glyph::tab_room, "room"},
	{0, Glyph::tab_new_content, "new-content"},
	{0, Glyph::tab_status, "status"},
	{0, Glyph::tab_alert, "alert"},
}};

inline constexpr std::array<IconStrip, 4> kIconStrips{{
	{IDR_MAINFRAME, "res/toolbar.bmp", kMainToolbarIcons.data(), kMainToolbarIcons.size()},
	{IDR_TEXTTOOLBAR, "res/texttool.bmp", kTextToolbarIcons.data(), kTextToolbarIcons.size()},
	{IDR_USERTOOLBAR, "res/usertool.bmp", kUserToolbarIcons.data(), kUserToolbarIcons.size()},
	{IDB_TABS, "res/tabbar.bmp", kRoomTabIcons.data(), kRoomTabIcons.size()},
}};

constexpr const IconStrip* FindIconStrip(std::uint32_t resource)
{
	for (const auto& strip : kIconStrips)
		if (strip.resource == resource) return &strip;
	return nullptr;
}

constexpr int FindIconIndex(std::uint32_t resource, std::uint32_t command)
{
	const auto* strip = FindIconStrip(resource);
	if (!strip) return -1;
	for (std::size_t index = 0; index < strip->binding_count; ++index)
		if (strip->bindings[index].command == command && command != 0)
			return static_cast<int>(index);
	return -1;
}

} // namespace comic_chat::modern_ui

#endif
