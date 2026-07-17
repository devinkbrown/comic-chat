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

constexpr int kExpressionCount = 8;

struct ExpressionSizeBinding {
	int width;
	int height;
	UINT png_resources[kExpressionCount];
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

// This is the order CBodyCam has used since Microsoft's release. Resource IDs
// are written out rather than inferred so a reordered generator cannot silently
// change the meaning of an emotion index.
constexpr ExpressionSizeBinding kExpressionSizeBindings[] = {
	{20, 26, {IDR_MODERN_PNG_EXPR_HAPPY_20X26, IDR_MODERN_PNG_EXPR_COY_20X26,
		IDR_MODERN_PNG_EXPR_BORED_20X26, IDR_MODERN_PNG_EXPR_SCARED_20X26,
		IDR_MODERN_PNG_EXPR_SAD_20X26, IDR_MODERN_PNG_EXPR_ANGRY_20X26,
		IDR_MODERN_PNG_EXPR_SHOUT_20X26, IDR_MODERN_PNG_EXPR_LAUGH_20X26}},
	{25, 33, {IDR_MODERN_PNG_EXPR_HAPPY_25X33, IDR_MODERN_PNG_EXPR_COY_25X33,
		IDR_MODERN_PNG_EXPR_BORED_25X33, IDR_MODERN_PNG_EXPR_SCARED_25X33,
		IDR_MODERN_PNG_EXPR_SAD_25X33, IDR_MODERN_PNG_EXPR_ANGRY_25X33,
		IDR_MODERN_PNG_EXPR_SHOUT_25X33, IDR_MODERN_PNG_EXPR_LAUGH_25X33}},
	{30, 39, {IDR_MODERN_PNG_EXPR_HAPPY_30X39, IDR_MODERN_PNG_EXPR_COY_30X39,
		IDR_MODERN_PNG_EXPR_BORED_30X39, IDR_MODERN_PNG_EXPR_SCARED_30X39,
		IDR_MODERN_PNG_EXPR_SAD_30X39, IDR_MODERN_PNG_EXPR_ANGRY_30X39,
		IDR_MODERN_PNG_EXPR_SHOUT_30X39, IDR_MODERN_PNG_EXPR_LAUGH_30X39}},
	{40, 52, {IDR_MODERN_PNG_EXPR_HAPPY_40X52, IDR_MODERN_PNG_EXPR_COY_40X52,
		IDR_MODERN_PNG_EXPR_BORED_40X52, IDR_MODERN_PNG_EXPR_SCARED_40X52,
		IDR_MODERN_PNG_EXPR_SAD_40X52, IDR_MODERN_PNG_EXPR_ANGRY_40X52,
		IDR_MODERN_PNG_EXPR_SHOUT_40X52, IDR_MODERN_PNG_EXPR_LAUGH_40X52}},
	{50, 65, {IDR_MODERN_PNG_EXPR_HAPPY_50X65, IDR_MODERN_PNG_EXPR_COY_50X65,
		IDR_MODERN_PNG_EXPR_BORED_50X65, IDR_MODERN_PNG_EXPR_SCARED_50X65,
		IDR_MODERN_PNG_EXPR_SAD_50X65, IDR_MODERN_PNG_EXPR_ANGRY_50X65,
		IDR_MODERN_PNG_EXPR_SHOUT_50X65, IDR_MODERN_PNG_EXPR_LAUGH_50X65}},
	{60, 78, {IDR_MODERN_PNG_EXPR_HAPPY_60X78, IDR_MODERN_PNG_EXPR_COY_60X78,
		IDR_MODERN_PNG_EXPR_BORED_60X78, IDR_MODERN_PNG_EXPR_SCARED_60X78,
		IDR_MODERN_PNG_EXPR_SAD_60X78, IDR_MODERN_PNG_EXPR_ANGRY_60X78,
		IDR_MODERN_PNG_EXPR_SHOUT_60X78, IDR_MODERN_PNG_EXPR_LAUGH_60X78}},
	{80, 104, {IDR_MODERN_PNG_EXPR_HAPPY_80X104, IDR_MODERN_PNG_EXPR_COY_80X104,
		IDR_MODERN_PNG_EXPR_BORED_80X104, IDR_MODERN_PNG_EXPR_SCARED_80X104,
		IDR_MODERN_PNG_EXPR_SAD_80X104, IDR_MODERN_PNG_EXPR_ANGRY_80X104,
		IDR_MODERN_PNG_EXPR_SHOUT_80X104, IDR_MODERN_PNG_EXPR_LAUGH_80X104}},
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

bool CreateWicFactory(ComPtr<IWICImagingFactory>& factory)
{
	return SUCCEEDED(::CoCreateInstance(
		CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf())));
}

bool LoadPngResourceFrame(
	IWICImagingFactory* factory,
	UINT png_resource,
	UINT expected_width,
	UINT expected_height,
	ComPtr<IWICBitmapFrameDecode>& frame)
{
	if (!factory || !expected_width || !expected_height) return false;
	HINSTANCE resources = AfxGetResourceHandle();
	HRSRC source = ::FindResourceW(
		resources, MAKEINTRESOURCEW(png_resource), MAKEINTRESOURCEW(kRawDataResourceType));
	if (!source) return false;
	const DWORD source_size = ::SizeofResource(resources, source);
	HGLOBAL loaded = ::LoadResource(resources, source);
	BYTE* source_bytes = loaded ? static_cast<BYTE*>(::LockResource(loaded)) : nullptr;
	if (!source_bytes || source_size == 0) return false;

	ComPtr<IWICStream> stream;
	if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
		FAILED(stream->InitializeFromMemory(source_bytes, source_size))) return false;
	ComPtr<IWICBitmapDecoder> decoder;
	if (FAILED(factory->CreateDecoderFromStream(
		stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) return false;
	UINT frame_count = 0;
	if (FAILED(decoder->GetFrameCount(&frame_count)) || frame_count != 1) return false;
	frame.Reset();
	if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) return false;
	UINT source_width = 0;
	UINT source_height = 0;
	return SUCCEEDED(frame->GetSize(&source_width, &source_height)) &&
		source_width == expected_width && source_height == expected_height;
}

bool DecodePbgraPixels(
	IWICImagingFactory* factory,
	IWICBitmapFrameDecode* frame,
	const WICRect* source_region,
	int target_width,
	int target_height,
	std::vector<BYTE>& pixels)
{
	if (!factory || !frame || target_width <= 0 || target_height <= 0 ||
		target_width > 4096 || target_height > 4096) return false;

	IWICBitmapSource* source = frame;
	ComPtr<IWICBitmapClipper> clipper;
	if (source_region) {
		if (source_region->Width <= 0 || source_region->Height <= 0 ||
			FAILED(factory->CreateBitmapClipper(clipper.GetAddressOf())) ||
			FAILED(clipper->Initialize(frame, source_region))) return false;
		source = clipper.Get();
	}

	// Convert before interpolation. Scaling straight-alpha RGB causes colored
	// fringes around the transparent edges of the generated artwork.
	ComPtr<IWICFormatConverter> converter;
	if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
		FAILED(converter->Initialize(
			source, GUID_WICPixelFormat32bppPBGRA,
			WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
		return false;

	IWICBitmapSource* final_source = converter.Get();
	ComPtr<IWICBitmapScaler> scaler;
	UINT converted_width = 0;
	UINT converted_height = 0;
	if (FAILED(converter->GetSize(&converted_width, &converted_height))) return false;
	if (converted_width != static_cast<UINT>(target_width) ||
		converted_height != static_cast<UINT>(target_height)) {
		if (FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) ||
			FAILED(scaler->Initialize(
				converter.Get(), static_cast<UINT>(target_width),
				static_cast<UINT>(target_height), WICBitmapInterpolationModeFant)))
			return false;
		final_source = scaler.Get();
	}

	try {
		const std::size_t stride = static_cast<std::size_t>(target_width) * 4U;
		const std::size_t byte_count = stride * static_cast<std::size_t>(target_height);
		if (stride > (std::numeric_limits<UINT>::max)() ||
			byte_count > (std::numeric_limits<UINT>::max)()) return false;
		pixels.resize(byte_count);
		return SUCCEEDED(final_source->CopyPixels(
			nullptr, static_cast<UINT>(stride), static_cast<UINT>(byte_count),
			pixels.data()));
	} catch (const std::bad_alloc&) {
		return false;
	}
}

bool InstallAlphaImageList(
	CImageList& image_list,
	int cell_width,
	int cell_height,
	int image_count,
	const std::vector<BYTE>& strip_pixels)
{
	if (cell_width <= 0 || cell_height <= 0 || image_count <= 0 ||
		cell_width > (std::numeric_limits<int>::max)() / image_count) return false;
	const std::size_t expected = static_cast<std::size_t>(cell_width) *
		static_cast<std::size_t>(image_count) * static_cast<std::size_t>(cell_height) * 4U;
	if (strip_pixels.size() != expected) return false;

	BITMAPINFO bitmap_info{};
	bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info.bmiHeader.biWidth = cell_width * image_count;
	bitmap_info.bmiHeader.biHeight = -cell_height;
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
	// CreateDIBSection exposes memory directly. Synchronize the calling thread's
	// GDI batch before handing that memory back to a GDI image-list operation.
	if (!::GdiFlush()) return false;

	CImageList replacement;
	if (!replacement.Create(cell_width, cell_height, ILC_COLOR32, image_count, 1))
		return false;
	replacement.SetBkColor(CLR_NONE);
	if (::ImageList_Add(replacement.GetSafeHandle(), bitmap_handle, nullptr) != 0 ||
		replacement.GetImageCount() != image_count) return false;
	return ReplaceImageList(image_list, replacement);
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

	ScopedComInitialization com;
	if (!com.available()) return false;
	ComPtr<IWICImagingFactory> factory;
	if (!CreateWicFactory(factory)) return false;
	ComPtr<IWICBitmapFrameDecode> frame;
	const std::uint64_t expected_width =
		static_cast<std::uint64_t>(declared_source_cell_size) * image_count;
	if (expected_width > (std::numeric_limits<UINT>::max)() ||
		!LoadPngResourceFrame(
			factory.Get(), png_resource, static_cast<UINT>(expected_width),
			static_cast<UINT>(declared_source_cell_size), frame))
		return false;

	try {
		const std::size_t row_bytes = static_cast<std::size_t>(cell_size) * 4U;
		const std::size_t cell_bytes = row_bytes * static_cast<std::size_t>(cell_size);
		const std::size_t strip_stride = row_bytes * static_cast<std::size_t>(image_count);
		std::vector<BYTE> strip_pixels(strip_stride * static_cast<std::size_t>(cell_size));
		std::vector<BYTE> cell_pixels(cell_bytes);
		for (int index = 0; index < image_count; ++index) {
			const WICRect region{
				declared_source_cell_size * index,
				0,
				declared_source_cell_size,
				declared_source_cell_size,
			};
			if (!DecodePbgraPixels(
				factory.Get(), frame.Get(), &region, cell_size, cell_size, cell_pixels))
				return false;
			for (int row = 0; row < cell_size; ++row) {
				std::memcpy(
					strip_pixels.data() + static_cast<std::size_t>(row) * strip_stride +
						static_cast<std::size_t>(index) * row_bytes,
					cell_pixels.data() + static_cast<std::size_t>(row) * row_bytes,
					row_bytes);
			}
		}

		return InstallAlphaImageList(
			image_list, cell_size, cell_size, image_count, strip_pixels);
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

bool LoadOwnedIconFrame(UINT resource, int width, int height, HICON& icon)
{
	icon = nullptr;
	return width > 0 && height > 0 && SUCCEEDED(::LoadIconWithScaleDown(
		AfxGetResourceHandle(), MAKEINTRESOURCEW(resource), width, height, &icon)) &&
		icon != nullptr;
}

void DestroyOwnedIconPair(HICON& big_icon, HICON& small_icon)
{
	if (small_icon && small_icon != big_icon) ::DestroyIcon(small_icon);
	if (big_icon) ::DestroyIcon(big_icon);
	big_icon = nullptr;
	small_icon = nullptr;
}

COLORREF SourceMaskColor(UINT resource)
{
	switch (resource) {
	case IDB_TABS:
		return RGB(0, 255, 0);
	case IDB_MEMBER:
	case IDB_CONNECT:
	case IDB_OLDNEW:
	case IDB_INACTIVE:
	case IDB_ACTIVE:
		return RGB(0, 0, 255);
	case IDB_STOPPED:
		return RGB(0, 0, 128);
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

bool DecodeModernSingleCell(
	IWICImagingFactory* factory,
	const ModernStripBinding& binding,
	int cell_size,
	std::vector<BYTE>& pixels)
{
	if (binding.image_count != 1) return false;
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
		ComPtr<IWICBitmapFrameDecode> frame;
		const UINT source_size = static_cast<UINT>(kModernStripSizes[best]);
		if (LoadPngResourceFrame(
				factory, binding.png_resource_base + static_cast<UINT>(best),
				source_size, source_size, frame) &&
			DecodePbgraPixels(
				factory, frame.Get(), nullptr, cell_size, cell_size, pixels)) return true;
	}
	return false;
}

bool BuildModernOrderedImageList(
	CImageList& image_list,
	const UINT* legacy_resources,
	int resource_count,
	int cell_size)
{
	ScopedComInitialization com;
	if (!com.available()) return false;
	ComPtr<IWICImagingFactory> factory;
	if (!CreateWicFactory(factory)) return false;

	try {
		const std::size_t row_bytes = static_cast<std::size_t>(cell_size) * 4U;
		const std::size_t cell_bytes = row_bytes * static_cast<std::size_t>(cell_size);
		const std::size_t strip_stride = row_bytes * static_cast<std::size_t>(resource_count);
		std::vector<BYTE> strip_pixels(strip_stride * static_cast<std::size_t>(cell_size));
		std::vector<BYTE> cell_pixels;
		for (int index = 0; index < resource_count; ++index) {
			const ModernStripBinding* binding = FindModernStrip(legacy_resources[index]);
			if (!binding || binding->image_count != 1 ||
				!DecodeModernSingleCell(factory.Get(), *binding, cell_size, cell_pixels) ||
				cell_pixels.size() != cell_bytes) return false;
			for (int row = 0; row < cell_size; ++row) {
				std::memcpy(
					strip_pixels.data() + static_cast<std::size_t>(row) * strip_stride +
						static_cast<std::size_t>(index) * row_bytes,
					cell_pixels.data() + static_cast<std::size_t>(row) * row_bytes,
					row_bytes);
			}
		}
		return InstallAlphaImageList(
			image_list, cell_size, cell_size, resource_count, strip_pixels);
	} catch (const std::bad_alloc&) {
		return false;
	}
}

bool BuildLegacyOrderedImageList(
	CImageList& image_list,
	const UINT* legacy_resources,
	int resource_count,
	int cell_size)
{
	CImageList replacement;
	if (!replacement.Create(
			cell_size, cell_size, ILC_COLOR24 | ILC_MASK, resource_count, 1))
		return false;

	CWindowDC screen(nullptr);
	for (int index = 0; index < resource_count; ++index) {
		CBitmap source;
		if (!source.LoadBitmap(legacy_resources[index])) return false;
		BITMAP source_info{};
		if (!source.GetBitmap(&source_info) || source_info.bmWidth <= 0 ||
			source_info.bmHeight <= 0) return false;

		CDC source_dc;
		CDC target_dc;
		CBitmap scaled;
		if (!source_dc.CreateCompatibleDC(&screen) ||
			!target_dc.CreateCompatibleDC(&screen) ||
			!scaled.CreateCompatibleBitmap(&screen, cell_size, cell_size)) return false;
		CBitmap* old_source = source_dc.SelectObject(&source);
		CBitmap* old_target = target_dc.SelectObject(&scaled);
		const COLORREF mask = SourceMaskColor(legacy_resources[index]);
		target_dc.FillSolidRect(0, 0, cell_size, cell_size, mask);
		target_dc.SetStretchBltMode(COLORONCOLOR);
		const BOOL copied = target_dc.StretchBlt(
			0, 0, cell_size, cell_size, &source_dc,
			0, 0, source_info.bmWidth, source_info.bmHeight, SRCCOPY);
		source_dc.SelectObject(old_source);
		target_dc.SelectObject(old_target);
		if (!copied || replacement.Add(&scaled, mask) != index) return false;
	}
	if (replacement.GetImageCount() != resource_count) return false;
	return ReplaceImageList(image_list, replacement);
}

const ExpressionSizeBinding* FindExpressionSize(CSize face_size)
{
	for (const auto& binding : kExpressionSizeBindings)
		if (binding.width == face_size.cx && binding.height == face_size.cy)
			return &binding;
	return nullptr;
}

bool BuildModernExpressionImageList(
	CImageList& image_list,
	const ExpressionSizeBinding& binding)
{
	ScopedComInitialization com;
	if (!com.available()) return false;
	ComPtr<IWICImagingFactory> factory;
	if (!CreateWicFactory(factory)) return false;

	try {
		const std::size_t row_bytes = static_cast<std::size_t>(binding.width) * 4U;
		const std::size_t face_bytes = row_bytes * static_cast<std::size_t>(binding.height);
		const std::size_t strip_stride = row_bytes * kExpressionCount;
		std::vector<BYTE> strip_pixels(strip_stride * static_cast<std::size_t>(binding.height));
		std::vector<BYTE> face_pixels;
		for (int index = 0; index < kExpressionCount; ++index) {
			ComPtr<IWICBitmapFrameDecode> frame;
			if (!LoadPngResourceFrame(
					factory.Get(), binding.png_resources[index],
					static_cast<UINT>(binding.width), static_cast<UINT>(binding.height), frame) ||
				!DecodePbgraPixels(
					factory.Get(), frame.Get(), nullptr,
					binding.width, binding.height, face_pixels) ||
				face_pixels.size() != face_bytes) return false;
			for (int row = 0; row < binding.height; ++row) {
				std::memcpy(
					strip_pixels.data() + static_cast<std::size_t>(row) * strip_stride +
						static_cast<std::size_t>(index) * row_bytes,
					face_pixels.data() + static_cast<std::size_t>(row) * row_bytes,
					row_bytes);
			}
		}
		return InstallAlphaImageList(
			image_list, binding.width, binding.height, kExpressionCount, strip_pixels);
	} catch (const std::bad_alloc&) {
		return false;
	}
}

} // namespace

DpiAwareWindowIcons::~DpiAwareWindowIcons()
{
	DestroyOwnedIconPair(big_icon_, small_icon_);
}

bool ApplyDpiAwareWindowIcons(
	CWnd& window,
	UINT icon_resource,
	DpiAwareWindowIcons& owned_icons)
{
	const HWND handle = window.GetSafeHwnd();
	if (!handle) return false;
	const UINT dpi = static_cast<UINT>(DpiForWindow(handle));
	HICON big_icon = nullptr;
	HICON small_icon = nullptr;
	if (!LoadOwnedIconFrame(
			icon_resource, SystemMetricForDpi(SM_CXICON, dpi),
			SystemMetricForDpi(SM_CYICON, dpi), big_icon) ||
		!LoadOwnedIconFrame(
			icon_resource, SystemMetricForDpi(SM_CXSMICON, dpi),
			SystemMetricForDpi(SM_CYSMICON, dpi), small_icon)) {
		DestroyOwnedIconPair(big_icon, small_icon);
		return false;
	}

	window.SetIcon(big_icon, TRUE);
	window.SetIcon(small_icon, FALSE);
	DestroyOwnedIconPair(owned_icons.big_icon_, owned_icons.small_icon_);
	owned_icons.big_icon_ = big_icon;
	owned_icons.small_icon_ = small_icon;
	return true;
}

void ReleaseDpiAwareWindowIcons(
	CWnd& window,
	DpiAwareWindowIcons& owned_icons)
{
	if (window.GetSafeHwnd()) {
		window.SetIcon(nullptr, TRUE);
		window.SetIcon(nullptr, FALSE);
	}
	DestroyOwnedIconPair(owned_icons.big_icon_, owned_icons.small_icon_);
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

bool BuildOrderedImageList(
	CImageList& image_list,
	const UINT* legacy_resources,
	int resource_count,
	int cell_size,
	HWND themed_window)
{
	UNREFERENCED_PARAMETER(themed_window);
	if (!legacy_resources || resource_count <= 0 || resource_count > 16 ||
		cell_size <= 0 || cell_size > 512) return false;

	// A list is either entirely alpha PNGs or entirely source bitmaps. Never
	// publish a prefix of new states followed by legacy states: their indices
	// are part of the notification and automation model contract.
	if (BuildModernOrderedImageList(
			image_list, legacy_resources, resource_count, cell_size)) return true;
	return BuildLegacyOrderedImageList(
		image_list, legacy_resources, resource_count, cell_size);
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
	UNREFERENCED_PARAMETER(themed_window);
	const ExpressionSizeBinding* binding = FindExpressionSize(face_size);
	if (binding && BuildModernExpressionImageList(image_list, *binding)) return true;

	// CBodyCam selects the original eight Microsoft CDIB/RLE resources whenever
	// this list has no handle. All eight PNGs must decode before installation;
	// on any failure, also discard a stale list from an earlier DPI/theme pass.
	image_list.DeleteImageList();
	return false;
}

} // namespace comic_chat::modern_ui
