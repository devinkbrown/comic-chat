# Microsoft Comic Chat 2.5 UI Source Audit

**Date:** 2026-07-19  
**Authority:** the pinned external Comic Chat release  
**Purpose:** prevent the portable client from drifting from the released UI
contract while allowing DPI, Unicode, accessibility, persistence, and native
platform integration to be modernized.

## Audit coverage

The audit inventories all 313 imported source/resource files, searches the
complete tree for UI ownership and command routing, and deep-reads the classes
and resources that define visible geometry or interaction:

- `chat.rc`, `resource.h`, `defines.h`;
- `mainfrm`, `childfrm`, `chatbars`, `coolbar`, `tabbar`;
- `chatview`, `spltchat`, `pageview`, `textview`, `status`;
- `saywnd`, `memblst`, `bodycam`, `chatdoc`;
- the property-page, room/member, automation, notification, transfer,
  whisper, color/font and setup dialog owners.

Historical screenshots are a visual cross-check, not a substitute for source.
The 1024x734 Microsoft Chat 2.5 capture and published character sheet exposed
two concrete port defects: the missing emotion wheel and inverted Jordan
monochrome polarity. Both are now covered by tests.

## Fixed shell contract

| Surface | Microsoft source | Contract | Portable state |
|---|---|---|---|
| Menu | `chat.rc:154` | File, Edit, View, Format, Room, Member, Favorites, Window, Help | Rendered in source order |
| Coolbar | `chat.rc:108-150`, `chatbars.cpp` | Main, member and text toolbars with 16x16 command glyphs | Modern line glyphs rendered in source command/group order |
| Room tabs | `tabbar.cpp` | 29px control bar; status tab plus sorted room tabs | Multi-room tabs, active state, unread badges, pointer switching, and per-room drafts are implemented |
| Main split | `spltchat.cpp` | Conversation 80%, right pane 20%; ratio retained during resize | Implemented and tested |
| Comic right split | `spltchat.cpp` | Member list 30%, body camera 70%; ratio retained during resize | Implemented and tested |
| Composer split | `spltchat.cpp`, `saywnd.cpp` | Fixed 23px minimum say pane | Implemented and tested |
| Say actions | `saywnd.cpp` | Five 24px controls; 17px Say, Think, Whisper, Action, Sound glyphs | Modern semantic glyphs in source order and geometry |
| Status bar | `mainfrm.cpp` | Connection text plus member-count pane | Rendered as two logical panes |
| Comic/text view | `chatview.cpp` | Swap page/text buffer without changing shell contract | Implemented; local commands and state wired |
| History keys | `saywnd.cpp` | Page Up/Down in composer forwards to output history | Implemented |
| Focus order | `chatdoc.cpp:2273` | Tabs, output, input, members, then comic-only emotion control | Forward order implemented and tested; Shift+Tab awaits modifier events |

## Body camera contract

`bodycam.cpp` is an input widget, not a generic avatar preview.

- White character area above the wheel.
- Figure is height-fitted, bottom-aligned, and may be horizontally clipped;
  it is not width-fitted into a card.
- Wheel height is `min(pane width, 159px)` and disappears below 93px.
- Wheel background is RGB(210,210,210).
- Eight authored 20x26 face icons are positioned every PI/4.
- Neutral selector begins in the center; pointer drag and keyboard arrows alter
  angle/intensity; Home returns to neutral.
- Double-click above the wheel opens Character options.
- Context menu exposes Freeze and Send Expression.

Implemented now: composition, thresholds, original face resources, neutral
selector, source focus position, corrected avatar polarity, pointer selection,
pose preview, and explicit pose transmission. Remaining: keyboard manipulation,
freeze/send-expression commands, double-click, and context commands.

## Member-list contract

Source: `memblst.cpp`, `userinfo.cpp`, `chatdoc.cpp`.

- No invented pane title; the list begins at the pane edge.
- List and icon modes are distinct View menu commands.
- Role/status visuals, current selection, keyboard navigation and context menu
  own the member-action target.
- Tab into an unselected list focuses the first entry.

Implemented now: source pane placement, default comic icon mode with authored
40px character icons, text-mode roster rows, self highlight, departed state,
focus, pointer selection, and whisper targeting. Remaining: user-controlled
icon/list switching, keyboard roving, roles, scrolling, and context menus.

## Comic and text buffer contract

Source: `pageview.cpp`, `panel.cpp`, `textview.cpp`, `rtfctrl.cpp`.

Implemented now:

- the source-derived page/figure/backdrop/balloon renderer;
- comic/text shell switching;
- bounded history viewport and Page Up/Page Down;
- empty, connecting and live transcript states.

Remaining UI work:

- visible scrollbars with source paging/tail behavior;
- text selection/copy and formatted RTF-equivalent runs;
- comic page break/manual break behavior;
- text/context menus and print preview;
- status-window variant and independent per-room scroll/view mode.

## Dialog/resource inventory

`chat.rc` defines 40 dialog/property-page templates. Portable dialogs must
preserve field grouping, labels, validation and command results; their chrome
and layout may scale for DPI and accessibility.

### Connection and identity

- `IDD_SETUPDIALOG`, `IDD_SERVERSPAGE`, `IDD_SETTINGSPAGE`;
- `IDD_PERSONALPAGE_IRC`, `IDD_PASSWORD`, `IDD_NICKNAME`, `IDD_CHANNEL`;
- `IDD_CHARACTERPAGE`, `IDD_BACKGROUNDPAGE`, `IDD_COMICS_VIEW`;
- `IDD_TEXTFONTPAGE_IRC`, `IDD_SETTEXTFONT`, `IDD_CHOOSECOLOR`.

### Rooms and members

- `IDD_ROOMLIST`, `IDD_USERLIST`, `IDD_CHANNELPROP`, `IDD_CHANNELCREATE`;
- `IDD_CHANPASSWORD`, `IDD_KICK`, `IDD_BAN`, `IDD_INVITE`, `IDD_INVITATION`;
- `IDD_MOTD`, `IDD_AWAYDLG`, `IDD_WHISPERBOX`, `IDD_SOUND_DLG`.

### Automation and notifications

- `IDD_AUTOMATION_PAGE`, `IDD_RULESPAGE`, `IDD_EDITRULE`;
- `IDD_RULESETSPAGE`, `IDD_ADDTOSETS`, `IDD_CREATESET`;
- `IDD_RENAMELOADEDSET`, `IDD_RENAMESET`;
- `IDD_ADVANCEDEVENTPARAMS`, `IDD_ADVANCEDRULESETTINGS`;
- `IDD_NOTIFICATIONS`, `IDD_NOTIFICATIONUSERS`.

### Files and application

- `IDD_FILE_TRANSFER`, `IDD_ABOUTBOX`.

All 40 surfaces now have typed IDs, source dimensions, modal routing, keyboard
editing, and shared controls. Room, character, backdrop, nickname, away,
moderation, invite, and whisper confirmations invoke live commands. The
remaining template-specific field groupings, validation, and command results
are still implementation work; a registry alone is not full Microsoft parity.

## Modernization boundary

Allowed without changing the product contract:

- scale all 96-DPI measurements together;
- current Microsoft-neutral colors and visible focus treatment;
- modern program icons and button states while retaining source commands,
  grouping, dimensions and semantics;
- UTF-8/IME, clipboard and accessible semantic exposure;
- safe atomic files in place of Registry blobs;
- native Wayland/X11/Win32 input and presentation;
- safe file pickers and explicit consent for transfers.

Not allowed:

- replacing the chat buffer with a dashboard/feed layout;
- removing the body camera or emotion wheel;
- changing pane proportions because a different layout looks newer;
- inventing navigation rails, cards, a “comic desk,” or unrelated product
  metaphors;
- collapsing source commands into an incompatible minimal menu.

Character, backdrop, comic-panel and emotion-face art are product content, not
program chrome; they remain source-authored unless a separately proven art pack
is selected.

## Verification gates

- `zig fmt --check build.zig source_ui_assets.zig src`
- `zig build test`
- native build plus Linux and Windows cross-builds
- deterministic framebuffer assertions for every fixed geometry surface
- X11 and Wayland window captures compared with the Microsoft visual reference
- command/focus/pointer tests for each activated control
- all 40 source dialog contracts either implemented or explicitly classified
  as an approved obsolete integration before calling the UI complete
