#include "stdafx.h"
#include "resource.h"
#include "modernicons.h"
#include "modernui.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

namespace comic_chat::modern_ui {
namespace {

using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
using Microsoft::WRL::ComPtr;
constexpr WORD kRawDataResourceType = 10; // RT_RCDATA without TCHAR ambiguity.

struct ModernStripBinding {
	UINT legacy_resource;
	UINT png_resource_base;
	int image_count;
};

constexpr int kModernStripSizes[] = {16, 20, 24, 32, 40, 48};

constexpr ModernStripBinding kModernStripBindings[] = {
	{IDR_MAINFRAME, IDR_MODERN_PNG_TOOLBAR_16, 10},
	{IDB_TABS, IDR_MODERN_PNG_TABS_16, 4},
	{IDB_SAY_BAR, IDR_MODERN_PNG_SAY_16, 7},
	{IDB_MEMBER, IDR_MODERN_PNG_MEMBER_16, 5},
	{IDR_TEXTTOOLBAR, IDR_MODERN_PNG_TEXT_16, 7},
	{IDR_USERTOOLBAR, IDR_MODERN_PNG_USER_16, 7},
	{IDB_CONNECT, IDR_MODERN_PNG_CONNECT_16, 2},
	{IDB_OLDNEW, IDR_MODERN_PNG_OLDNEW_16, 1},
	{IDB_INACTIVE, IDR_MODERN_PNG_INACTIVE_16, 1},
	{IDB_ACTIVE, IDR_MODERN_PNG_ACTIVE_16, 1},
	{IDB_STOPPED, IDR_MODERN_PNG_STOPPED_16, 1},
};

const ModernStripBinding* FindModernStrip(UINT legacy_resource)
{
	for (const auto& binding : kModernStripBindings)
		if (binding.legacy_resource == legacy_resource) return &binding;
	return nullptr;
}

class ScopedComInitialization final {
public:
	ScopedComInitialization()
		: result_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
		  uninitialize_(SUCCEEDED(result_)) {}
	~ScopedComInitialization() { if (uninitialize_) ::CoUninitialize(); }
	ScopedComInitialization(const ScopedComInitialization&) = delete;
	ScopedComInitialization& operator=(const ScopedComInitialization&) = delete;
	bool available() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
private:
	HRESULT result_;
	bool uninitialize_;
};

bool ReplaceImageList(CImageList& target, CImageList& replacement)
{
	HIMAGELIST replacement_handle = replacement.Detach();
	if (!replacement_handle) return false;
	HIMAGELIST previous_handle = target.Detach();
	if (!target.Attach(replacement_handle)) {
		::ImageList_Destroy(replacement_handle);
		if (previous_handle && !target.Attach(previous_handle))
			::ImageList_Destroy(previous_handle);
		return false;
	}
	if (previous_handle) ::ImageList_Destroy(previous_handle);
	return true;
}

bool BuildPngStripImageList(
	CImageList& image_list,
	UINT png_resource,
	int declared_source_cell_size,
	int cell_size,
	int image_count)
{
	if (declared_source_cell_size <= 0 || cell_size <= 0 || cell_size > 512 ||
		image_count <= 0 || image_count > 64 ||
		cell_size > (std::numeric_limits<int>::max)() / image_count)
		return false;

	HINSTANCE resources = AfxGetResourceHandle();
	HRSRC source = ::FindResourceW(
		resources, MAKEINTRESOURCEW(png_resource), MAKEINTRESOURCEW(kRawDataResourceType));
	if (!source) return false;
	const DWORD source_size = ::SizeofResource(resources, source);
	HGLOBAL loaded = ::LoadResource(resources, source);
	BYTE* source_bytes = loaded ? static_cast<BYTE*>(::LockResource(loaded)) : nullptr;
	if (!source_bytes || source_size == 0) return false;

	ScopedComInitialization com;
	if (!com.available()) return false;
	ComPtr<IWICImagingFactory> factory;
	if (FAILED(::CoCreateInstance(
		CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())))) return false;
	ComPtr<IWICStream> stream;
	if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
		FAILED(stream->InitializeFromMemory(source_bytes, source_size))) return false;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromStream(
		stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return false;
	UINT frame_count = 0;
	if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count != 1) return false;
	ComPtr<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;
	UINT source_width = 0;
	UINT source_height = 0;
	if (FAILED(frame->GetSize(&source_width, &source_height)) ||
		source_height != static_cast<UINT>(declared_source_cell_size) ||
		source_width != static_cast<std::uint64_t>(source_height) * image_count)
		return false;

	try {
		const std::size_t row_bytes = static_cast<std::size_t>(cell_size) * 4U;
		const std::size_t cell_bytes = row_bytes * static_cast<std::size_t>(cell_size);
		const std::size_t strip_stride = row_bytes * static_cast<std::size_t>(image_count);
		std::vector<BYTE> strip_pixels(strip_stride * static_cast<std::size_t>(cell_size));
		std::vector<BYTE> cell_pixels(cell_bytes);
		for (int index = 0; index < image_count; ++index) {
			ComPtr<IWICBitmapClipper> clipper;
			if (FAILED(factory->CreateBitmapClipper(clipper.GetAddressOf()))) return false;
			const WICRect region{
				static_cast<INT>(static_cast<std::uint64_t>(source_height) * index),
				0,
				static_cast<INT>(source_height),
				static_cast<INT>(source_height),
			};
			if (FAILED(clipper->Initialize(frame.Get(), &region))) return false;

			// Convert to premultiplied BGRA before interpolation. Scaling straight
			// alpha would bleed hidden RGB into translucent icon edges.
			ComPtr<IWICFormatConverter> converter;
			if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
				FAILED(converter->Initialize(
					clipper.Get(), GUID_WICPixelFormat32bppPBGRA,
					WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
				return false;

			IWICBitmapSource* scaled_source = converter.Get();
			ComPtr<IWICBitmapScaler> scaler;
			if (source_height != static_cast<UINT>(cell_size)) {
				if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
					FAILED(scaler->Initialize(
						converter.Get(), static_cast<UINT>(cell_size), static_cast<UINT>(cell_size),
						WICBitmapInterpolationModeFant))) return false;
				scaled_source = scaler.Get();
			}

			if (FAILED(scaled_source->CopyPixels(
					nullptr, static_cast<UINT>(row_bytes), static_cast<UINT>(cell_bytes),
					cell_pixels.data()))) return false;
			for (int row = 0; row < cell_size; ++row) {
				std::memcpy(
					strip_pixels.data() + static_cast<std::size_t>(row) * strip_stride +
						static_cast<std::size_t>(index) * row_bytes,
					cell_pixels.data() + static_cast<std::size_t>(row) * row_bytes,
					row_bytes);
			}
		}

		BITMAPINFO bitmap_info{};
		bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmap_info.bmiHeader.biWidth = cell_size * image_count;
		bitmap_info.bmiHeader.biHeight = -cell_size;
		bitmap_info.bmiHeader.biPlanes = 1;
		bitmap_info.bmiHeader.biBitCount = 32;
		bitmap_info.bmiHeader.biCompression = BI_RGB;
		void* bitmap_bits = nullptr;
		HBITMAP bitmap_handle = ::CreateDIBSection(
			nullptr, &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
		if (!bitmap_handle || !bitmap_bits) {
			if (bitmap_handle) ::DeleteObject(bitmap_handle);
			return false;
		}
		CBitmap bitmap;
		bitmap.Attach(bitmap_handle);
		std::memcpy(bitmap_bits, strip_pixels.data(), strip_pixels.size());

		CImageList replacement;
		if (!replacement.Create(cell_size, cell_size, ILC_COLOR32, image_count, 1))
			return false;
		replacement.SetBkColor(CLR_NONE);
		if (::ImageList_Add(replacement.GetSafeHandle(), bitmap_handle, nullptr) != 0 ||
			replacement.GetImageCount() != image_count) {
			return false;
		}
		return ReplaceImageList(image_list, replacement);
	} catch (const std::bad_alloc&) {
		return false;
	}
}

bool BuildModernStripImageList(
	CImageList& image_list,
	const ModernStripBinding& binding,
	int cell_size)
{
	bool attempted[std::size(kModernStripSizes)]{};
	for (std::size_t attempt = 0; attempt < std::size(kModernStripSizes); ++attempt) {
		std::size_t best = std::size(kModernStripSizes);
		int best_distance = (std::numeric_limits<int>::max)();
		for (std::size_t index = 0; index < std::size(kModernStripSizes); ++index) {
			if (attempted[index]) continue;
			const int distance = kModernStripSizes[index] > cell_size
				? kModernStripSizes[index] - cell_size
				: cell_size - kModernStripSizes[index];
			if (best == std::size(kModernStripSizes) || distance < best_distance ||
				(distance == best_distance && kModernStripSizes[index] > kModernStripSizes[best])) {
				best = index;
				best_distance = distance;
			}
		}
		attempted[best] = true;
		if (BuildPngStripImageList(
				image_list, binding.png_resource_base + static_cast<UINT>(best),
				kModernStripSizes[best], cell_size, binding.image_count)) return true;
	}
	return false;
}

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
	if (const ModernStripBinding* modern = FindModernStrip(legacy_resource)) {
		// The mapping is also the semantic cell-count contract. In particular,
		// the seven say/think cells retain their original command indices even
		// though the modern PNG uses equal square cells.
		if (modern->image_count != image_count) return false;
		if (BuildModernStripImageList(image_list, *modern, cell_size)) return true;
	}

	// Until every generated strip is embedded, missing or invalid modern data
	// falls back to Microsoft's released RT_BITMAP with its exact cell topology.
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
	// COLORONCOLOR intentionally preserves the fallback source palette and exact
	// pixel relationships.
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

	CImageList replacement;
	if (!replacement.Create(
			cell_size, cell_size, ILC_COLOR24 | ILC_MASK, image_count, 1) ||
		replacement.Add(&scaled, mask) < 0 ||
		replacement.GetImageCount() != image_count) {
		return false;
	}
	return ReplaceImageList(image_list, replacement);
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
