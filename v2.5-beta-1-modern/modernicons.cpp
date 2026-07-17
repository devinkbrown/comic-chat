#include "stdafx.h"
#include "resource.h"
#include "modernicons.h"
#include "modernui.h"

namespace comic_chat::modern_ui {
namespace {

using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);

int SystemMetricForDpi(int metric, UINT dpi)
{
	if (HMODULE user = ::GetModuleHandleW(L"user32.dll")) {
		if (const auto get_metric = reinterpret_cast<GetSystemMetricsForDpiFn>(
			::GetProcAddress(user, "GetSystemMetricsForDpi"))) {
			const int value = get_metric(metric, dpi);
			if (value > 0) return value;
		}
	}
	const int system_value = ::GetSystemMetrics(metric);
	const UINT system_dpi = SystemDpi();
	const int scaled = ::MulDiv(system_value, static_cast<int>(dpi),
		static_cast<int>(system_dpi ? system_dpi : 96U));
	return scaled > 0 ? scaled : 1;
}

HICON LoadSharedIconFrame(UINT resource, int width, int height)
{
	return reinterpret_cast<HICON>(::LoadImage(
		AfxGetResourceHandle(), MAKEINTRESOURCE(resource), IMAGE_ICON,
		width, height, LR_SHARED));
}

COLORREF SourceMaskColor(UINT resource)
{
	switch (resource) {
	case IDB_TABS:
		return RGB(0, 255, 0);
	case IDB_MEMBER:
		return RGB(0, 0, 255);
	default:
		return RGB(192, 192, 192);
	}
}

bool SourceCellBounds(
	UINT resource,
	const BITMAP& source,
	int image_count,
	int index,
	int& left,
	int& width)
{
	// The Microsoft say/think strip is a historical oddity: six 17-pixel
	// cells followed by a final 16-pixel cell (118 x 17 total).  Treat those
	// physical cells independently; dividing the whole strip by seven either
	// rejects the original bitmap or lets scaling bleed adjacent glyphs.
	static const int kSayBarOffsets[] = { 0, 17, 34, 51, 68, 85, 102 };
	static const int kSayBarWidths[] = { 17, 17, 17, 17, 17, 17, 16 };
	if (resource == IDB_SAY_BAR && source.bmWidth == 118 &&
		source.bmHeight == 17 && image_count == 7) {
		left = kSayBarOffsets[index];
		width = kSayBarWidths[index];
		return true;
	}

	if (source.bmWidth % image_count != 0)
		return false;
	width = source.bmWidth / image_count;
	left = index * width;
	return width > 0;
}

} // namespace

bool ApplyDpiAwareWindowIcons(CWnd& window, UINT icon_resource)
{
	const HWND handle = window.GetSafeHwnd();
	if (!handle) return false;
	const UINT dpi = static_cast<UINT>(DpiForWindow(handle));
	const HICON big_icon = LoadSharedIconFrame(
		icon_resource, SystemMetricForDpi(SM_CXICON, dpi),
		SystemMetricForDpi(SM_CYICON, dpi));
	const HICON small_icon = LoadSharedIconFrame(
		icon_resource, SystemMetricForDpi(SM_CXSMICON, dpi),
		SystemMetricForDpi(SM_CYSMICON, dpi));
	if (!big_icon || !small_icon) return false;
	window.SetIcon(big_icon, TRUE);
	window.SetIcon(small_icon, FALSE);
	return true;
}

bool BuildStripImageList(
	CImageList& image_list,
	UINT legacy_resource,
	int cell_size,
	int image_count,
	HWND themed_window)
{
	UNREFERENCED_PARAMETER(themed_window);
	if (cell_size <= 0 || image_count <= 0)
		return false;

	// Only the original Microsoft RT_BITMAP resource is legal here.  Rejected
	// generated PNG/RCDATA artwork is deliberately not addressable by this path.
	CBitmap source;
	if (!source.LoadBitmap(legacy_resource))
		return false;
	BITMAP source_info = {};
	if (!source.GetBitmap(&source_info) || source_info.bmWidth <= 0 ||
		source_info.bmHeight <= 0)
		return false;
	int final_left = 0;
	int final_width = 0;
	if (!SourceCellBounds(legacy_resource, source_info, image_count,
		image_count - 1, final_left, final_width) ||
		final_left + final_width != source_info.bmWidth)
		return false;

	CWindowDC screen(nullptr);
	CDC source_dc;
	CDC target_dc;
	CBitmap scaled;
	if (!source_dc.CreateCompatibleDC(&screen) ||
		!target_dc.CreateCompatibleDC(&screen) ||
		!scaled.CreateCompatibleBitmap(&screen, cell_size * image_count, cell_size))
		return false;

	CBitmap* old_source = source_dc.SelectObject(&source);
	CBitmap* old_target = target_dc.SelectObject(&scaled);
	const COLORREF mask = SourceMaskColor(legacy_resource);
	target_dc.FillSolidRect(0, 0, cell_size * image_count, cell_size, mask);
	// COLORONCOLOR intentionally preserves the source palette and exact pixel
	// relationships.  It cannot introduce any rejected generated illustration.
	target_dc.SetStretchBltMode(COLORONCOLOR);
	BOOL copied = TRUE;
	for (int index = 0; index < image_count; ++index) {
		int source_left = 0;
		int source_width = 0;
		if (!SourceCellBounds(legacy_resource, source_info, image_count,
			index, source_left, source_width) ||
			!target_dc.StretchBlt(
				index * cell_size, 0, cell_size, cell_size,
				&source_dc, source_left, 0, source_width,
				source_info.bmHeight, SRCCOPY)) {
			copied = FALSE;
			break;
		}
	}
	source_dc.SelectObject(old_source);
	target_dc.SelectObject(old_target);
	if (!copied)
		return false;

	image_list.DeleteImageList();
	if (!image_list.Create(
		cell_size, cell_size, ILC_COLOR24 | ILC_MASK, image_count, 1) ||
		image_list.Add(&scaled, mask) < 0 ||
		image_list.GetImageCount() != image_count) {
		image_list.DeleteImageList();
		return false;
	}
	return true;
}

CSize ExpressionFaceSizeForDpi(UINT dpi)
{
	if (dpi <= 108) return CSize(20, 26);
	if (dpi <= 132) return CSize(25, 33);
	if (dpi <= 168) return CSize(30, 39);
	if (dpi <= 216) return CSize(40, 52);
	if (dpi <= 264) return CSize(50, 65);
	if (dpi <= 336) return CSize(60, 78);
	return CSize(80, 104);
}

bool BuildExpressionImageList(
	CImageList& image_list,
	CSize face_size,
	HWND themed_window)
{
	UNREFERENCED_PARAMETER(face_size);
	UNREFERENCED_PARAMETER(themed_window);
	// CBodyCam interprets FALSE as "draw Icons.GetIcon()", the original eight
	// Microsoft CDIB/RLE resources.  Delete first so no stale generated list can
	// survive a DPI or theme refresh in a long-running process.
	image_list.DeleteImageList();
	return false;
}

} // namespace comic_chat::modern_ui
