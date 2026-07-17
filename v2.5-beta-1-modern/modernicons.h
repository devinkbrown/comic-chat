#ifndef COMIC_CHAT_MODERN_ICONS_H
#define COMIC_CHAT_MODERN_ICONS_H

#include <afxcmn.h>

namespace comic_chat::modern_ui {

// Load independent large and small frames from a multi-image icon resource.
// LR_SHARED leaves ownership with USER32, so the receiving window can retain
// both handles for its complete lifetime without leaking or destroying them.
bool ApplyDpiAwareWindowIcons(CWnd& window, UINT icon_resource);

// Rebuild a DPI-sized image list from a mapped alpha PNG resource when one is
// embedded. The original Microsoft TOOLBAR/BITMAP remains the temporary,
// source-faithful fallback and command/index order never changes.
bool BuildStripImageList(
	CImageList& image_list,
	UINT legacy_resource,
	int cell_size,
	int image_count,
	HWND themed_window);

// Build explicitly ordered one-cell states as one atomic list. The modern path
// is all-or-nothing; missing PNG data falls back to the complete ordered set of
// original Microsoft bitmaps without changing state indices.
bool BuildOrderedImageList(
	CImageList& image_list,
	const UINT* legacy_resources,
	int resource_count,
	int cell_size,
	HWND themed_window);

// Bodycam metrics and expression PNG resources use the same declared DPI size
// tiers. Missing or invalid generated data leaves the list empty so CBodyCam
// draws Microsoft's original eight CDIBs.
CSize ExpressionFaceSizeForDpi(UINT dpi);
bool BuildExpressionImageList(
	CImageList& image_list,
	CSize face_size,
	HWND themed_window);

} // namespace comic_chat::modern_ui

#endif
