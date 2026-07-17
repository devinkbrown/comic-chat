#ifndef COMIC_CHAT_MODERN_ICONS_H
#define COMIC_CHAT_MODERN_ICONS_H

#include <afxcmn.h>

namespace comic_chat::modern_ui {

// Load independent large and small frames from a multi-image icon resource.
// LR_SHARED leaves ownership with USER32, so the receiving window can retain
// both handles for its complete lifetime without leaking or destroying them.
bool ApplyDpiAwareWindowIcons(CWnd& window, UINT icon_resource);

// Rebuild a DPI-sized image list exclusively from the original Microsoft
// TOOLBAR/BITMAP resource. Command/index order and source pixels are unchanged.
bool BuildStripImageList(
	CImageList& image_list,
	UINT legacy_resource,
	int cell_size,
	int image_count,
	HWND themed_window);

// Bodycam metrics remain DPI-aware. BuildExpressionImageList intentionally
// returns false so the caller draws the original eight Microsoft CDIBs.
CSize ExpressionFaceSizeForDpi(UINT dpi);
bool BuildExpressionImageList(
	CImageList& image_list,
	CSize face_size,
	HWND themed_window);

} // namespace comic_chat::modern_ui

#endif
