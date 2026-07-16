#include "stdafx.h"
#include "bbox.h"
#include "chat.h"		// for theApp
#include "dib.h"
//#include "chatprot.h"	// for AVBMPPATH
#include "vector2d.h"  // only for PI
#include "pe.h"
#include "avatar.h"
#include "avatario.h"
#include "backdrop.h"
#include <limits>

// NOTE: Everything from here down should match with avbfile.cpp in the avtools
// directory. The only thing different between the two files are the includes.
					 
static const COLORREF MonochromePalette[] = { RGB(255,255,255), RGB(0,0,0) };
static const COLORREF MaskedMonoPalette[] = { RGB(255,255,255), RGB(0,0,0), RGB(128,0,0), RGB(0,0,128) };

// Avatar offsets and accumulated adjustments are attacker-controlled file data.
// Keep the legacy zero sentinel, but reject arithmetic that leaves the stream's
// representable range instead of wrapping into an unrelated record.
static BOOL AdjustAvatarOffset(DWORD& dwOffset, long lBy)
{
	if (dwOffset == 0)
		return TRUE;
	const long long adjusted = static_cast<long long>(dwOffset) + static_cast<long long>(lBy);
	if (adjusted <= 0 || adjusted > static_cast<long long>((std::numeric_limits<DWORD>::max)()))
		return FALSE;
	dwOffset = static_cast<DWORD>(adjusted);
	return TRUE;
}

static BOOL AddResourceAdjustment(long& current, int delta)
{
	const long long adjusted = static_cast<long long>(current) + static_cast<long long>(delta);
	if (adjusted < static_cast<long long>((std::numeric_limits<long>::min)()) ||
		adjusted > static_cast<long long>((std::numeric_limits<long>::max)()))
		return FALSE;
	current = static_cast<long>(adjusted);
	return TRUE;
}

static BOOL IsValidAvatarBitmap(const BITMAPINFOHEADER& header)
{
	if (header.biPlanes != 1 || header.biWidth <= 0 || header.biHeight == 0 ||
		header.biWidth > MAX_AVATAR_IMAGE_DIMENSION ||
		header.biHeight == (std::numeric_limits<LONG>::min)())
		return FALSE;
	const long height = header.biHeight < 0 ? -header.biHeight : header.biHeight;
	if (height > MAX_AVATAR_IMAGE_DIMENSION)
		return FALSE;
	switch (header.biBitCount)
	{
		case 1:
		case 4:
		case 8:
		case 16:
		case 24:
		case 32:
			break;
		default:
			return FALSE;
	}
	if (header.biCompression == BI_RLE4)
		return header.biBitCount == 4 && header.biHeight > 0;
	if (header.biCompression == BI_RLE8)
		return header.biBitCount == 8 && header.biHeight > 0;
	return header.biCompression == BI_RGB || header.biCompression == BI_BITFIELDS;
}

// ============================================================================
// CAvatarStream implementation

// Read a string, up to a given number of characters, from a stream.

BOOL
CAvatarStream::ReadString(
LPTSTR pszVal, 
UINT cbBufMax)
{
	if (pszVal == NULL || cbBufMax < 2 * sizeof(TCHAR))
		return FALSE;
	cbBufMax -= cbBufMax % sizeof(TCHAR);

	// Safe reader that doesn't overwrite any buffers.
	ASSERT (pszVal != NULL);
	ASSERT (cbBufMax > 0);
	UINT cchRemaining = cbBufMax / sizeof(TCHAR);
	while (cchRemaining > 1) {
		TCHAR tch;
		if (Read (&tch, sizeof(tch)) != sizeof(tch)) {
			*pszVal = '\0';
			return FALSE;
		}
		*(pszVal++) = tch;
		--cchRemaining;
		if (tch == '\0')
			return TRUE;
	}
	*pszVal = '\0';
	return FALSE;
}

#if defined(AVATAR_READ)

// Reads a buffer to the stream with zlib decompression. The buffer is 
// freshly allocated.

BOOL 
CAvatarStream::AllocAndReadCompressedBuffer(
void * * pvData, 
UINT * pcbData)
{
	ASSERT (pvData != NULL && pcbData != NULL);
	if (pvData == NULL || pcbData == NULL)
		return FALSE;
	*pvData = NULL;
	*pcbData = 0;
	
	struct
	{
		DWORD dwUncompressedSize;
		DWORD dwCompressedSize;
	} sizes;
	if (Read (&sizes, sizeof(sizes)) != sizeof(sizes)) {
		return FALSE;
	}

	if (sizes.dwUncompressedSize == 0) {
		return sizes.dwCompressedSize == 0;
	}
	
	// Sanity check for bad files.
	if (sizes.dwCompressedSize == 0 || sizes.dwUncompressedSize > MAX_COMPRESSBUFFERSIZE ||
		sizes.dwCompressedSize > MAX_COMPRESSBUFFERSIZE) {
		TRACE("Too big a buffer to read - returning failure");
		return FALSE;
	}

	// Allocate both buffers.
	PBYTE pbAllocUncompressed, pbAllocCompressed;
	pbAllocUncompressed = (PBYTE)malloc (sizes.dwUncompressedSize);
	pbAllocCompressed = (PBYTE)malloc (sizes.dwCompressedSize);
	if (pbAllocUncompressed == NULL || pbAllocCompressed == NULL) {
		TRACE("Failed to allocate memory.");
		free (pbAllocUncompressed);
		free (pbAllocCompressed);
		return FALSE;
	}

	if (Read (pbAllocCompressed, sizes.dwCompressedSize) != sizes.dwCompressedSize) {
		TRACE("Failed to read buffer");
		goto $abort;
	}

	if (ZLIB::uncompress (pbAllocUncompressed, &sizes.dwUncompressedSize, pbAllocCompressed, sizes.dwCompressedSize) != 0) {
		TRACE("ZLIB decompression failed!");
		goto $abort;
	}

	// Free the temporary buffer. The other one will be returned to the caller.
	free (pbAllocCompressed);
	*pvData = pbAllocUncompressed;
	*pcbData = sizes.dwUncompressedSize;
	return TRUE;

   $abort:
	// Error!
	free (pbAllocUncompressed);
    free (pbAllocCompressed);
	return FALSE;
}

#endif // AVATAR_READ

// ============================================================================
// CAvatarFileStream implementation

// Construct a file stream, from a filename.

CAvatarFileStream::CAvatarFileStream(
LPCTSTR pszFile, 
BOOL bWrite)
{
	m_nOpenCount = 0;
	m_file = NULL;
   #if defined(AVATAR_WRITE)
    m_bWrite = bWrite;
   #else
    ASSERT (!bWrite);
   #endif // !AVATAR_WRITE
	m_szFileName[0] = '\0';
	if (pszFile == NULL || lstrlen(pszFile) >= _countof(m_szFileName))
		return;
	lstrcpyn(m_szFileName, pszFile, _countof(m_szFileName));
}
   
CAvatarFileStream::~CAvatarFileStream()
{
	// Close the file if still open.
	if (m_nOpenCount > 0) {
		m_nOpenCount = 1;
		Close ();
	}
}

// Open the file, if not already open. Otherwise, add a reference to it.

BOOL
CAvatarFileStream::Open()
{
	if (m_szFileName[0] == '\0')
		return FALSE;
	// If already open, just return success. The file is ref-counted.
	if (m_nOpenCount > 0) {
		if (m_nOpenCount == (std::numeric_limits<UINT>::max)())
			return FALSE;
		m_nOpenCount++;
		return TRUE;
	}

   #if defined(AVATAR_WRITE)
	m_file = _tfopen (m_szFileName, m_bWrite ? __T("wb") : __T("rb"));
   #else // AVATAR_WRITE
	m_file = _tfopen (m_szFileName, __T("rb"));
   #endif // !AVATAR_WRITE
   
    if (m_file != NULL)
	{
		m_nOpenCount = 1;
	}
	return m_file != NULL;
}

// Close a reference to the file, closing the file if this was the last reference.

BOOL
CAvatarFileStream::Close()
{
	ASSERT(m_nOpenCount > 0);
	if (m_nOpenCount == 0 || m_file == NULL)
		return FALSE;

	// If the file is multiply opened, just decrease the ref. count.
	if (m_nOpenCount > 1) {
		m_nOpenCount--;
		return TRUE;
	}

	fclose (m_file);
	m_file = NULL;
	m_nOpenCount = 0;
	return TRUE;
}

// Read data from a stream, returning the number of bytes read,
// or AVSTREAM_ERROR for an error.

UINT 
CAvatarFileStream::Read(
LPVOID pvData, 
UINT cbData)
{
	return m_file != NULL ? fread (pvData, 1, cbData, m_file) : (UINT)AVSTREAM_ERROR;
}

// Return the current position in the stream, or AVSTREAM_ERROR for an error.

long 
CAvatarFileStream::GetPosition()
{
	return m_file != NULL ? ftell (m_file) : AVSTREAM_ERROR;
}

// Sets the current position in the stream, returning TRUE or FALSE.
// The nFrom values are the same as standard stream I/O (SEEK_SET etc.)

BOOL
CAvatarFileStream::SetPosition(
long lPosition, 
UINT nFrom)
{
	return m_file != NULL ? fseek (m_file, lPosition, nFrom) == 0 : FALSE;
}


// ============================================================================
// CAvatarPalette implementation

// Load a palette from the current position in the stream.

#if defined(AVATAR_READ)

BOOL 
CAvatarPalette::Read(
CAvatarStream * pStream)
{
	// Should not have already read this structure.
	ASSERT (m_pclrref == NULL);

	AVBINT16 nEntries;
	if (!pStream->Read16 (&nEntries)) {
		return FALSE;
	}
	
	// Check some reasonable bounds, for tricky files.
	if (nEntries > MAX_PALETTE_SIZE) {
		TRACE ("Palette abnormally big - returning failure");
		return FALSE;
	}
	
	if (nEntries > 0) {
		// The entries are stored as three-byte values. This is the most optimal
		// way of saving them, since they don't compress very well.
		m_pclrref = (COLORREF *)malloc (nEntries * sizeof(COLORREF));
		if (m_pclrref == NULL) {
			return FALSE;
		}
		ZeroMemory (m_pclrref, nEntries * sizeof(COLORREF));
		for (int i = 0; i < nEntries; i++) {
			if (pStream->Read (m_pclrref + i, 3) != 3) {
				TRACE("Error reading palette entries");
				free(m_pclrref);
				m_pclrref = NULL;
				return FALSE;
			}
		}
	}

	m_nColorCount = nEntries;
	return TRUE;
}

#endif // AVATAR_READ

// Sets the palette's entries from an array and a count.

BOOL 
CAvatarPalette::SetFrom(
const COLORREF * pclrrefSrc, 
int nCount)
{
	ASSERT(nCount == 0 || pclrrefSrc != NULL);
	ASSERT(m_pclrref == NULL);
	if (nCount < 0 || nCount > MAX_PALETTE_SIZE || (nCount > 0 && pclrrefSrc == NULL) ||
		m_pclrref != NULL)
		return FALSE;
	m_nColorCount = nCount;
	if (nCount > 0) {
		m_pclrref = (COLORREF *)malloc (nCount * sizeof(COLORREF));
		if (m_pclrref == NULL) {
			return FALSE;
		}
		memcpy (m_pclrref, pclrrefSrc, nCount * sizeof(COLORREF));
	}
	return TRUE;
}


// ============================================================================
// CAvatarDIB implementation

// Load a DIB from the current position in the stream.

BOOL 
CAvatarDIB::Load(
CAvatarStream * pStream)
{
	BOOL bIsPM = FALSE;
	BITMAPINFO* pBmpInfo = NULL;
	BYTE* pBits = NULL;
	DWORD dwBitsSize;
	unsigned long long bitsPosition;
	unsigned long long decodedSize;
	long bitmapHeight;

    // Get the current file position.
    DWORD dwFileStart = (DWORD)pStream->GetPosition ();
	if (dwFileStart == (DWORD)AVSTREAM_ERROR)
		return FALSE;

    // Read the file header to get the file size and to
    // find where the bits start in the file.
    BITMAPFILEHEADER BmpFileHdr;
    int iBytes;
    iBytes = pStream->Read (&BmpFileHdr, sizeof(BmpFileHdr));
    if (iBytes != sizeof(BmpFileHdr)) {
        TRACE("Failed to read file header");
        goto $abort;
    }

    // Check that we have the magic 'BM' at the start.
    if (BmpFileHdr.bfType != 0x4D42) {
        TRACE("Not a bitmap file");
        goto $abort;
    }

    // Make a wild guess that the file is in Windows DIB
    // format and read the BITMAPINFOHEADER. If the file turns
    // out to be a PM DIB file we'll convert it later.
    BITMAPINFOHEADER BmpInfoHdr;
    iBytes = pStream->Read (&BmpInfoHdr, sizeof(BmpInfoHdr));
    if (iBytes != sizeof(BmpInfoHdr)) {
        TRACE("Failed to read BITMAPINFOHEADER");
        goto $abort;
    }

    // Check that we got a real Windows DIB file.
    if (BmpInfoHdr.biSize != sizeof(BITMAPINFOHEADER)) {
        if (BmpInfoHdr.biSize != sizeof(BITMAPCOREHEADER)) {
            TRACE(" File is not Windows or PM DIB format");
            goto $abort;
        }

        // Set a flag to convert PM file to Win format later.
        bIsPM = TRUE;

        // Back up the file pointer and read the BITMAPCOREHEADER
        // and create the BITMAPINFOHEADER from it.
		const unsigned long long coreHeaderPosition =
			static_cast<unsigned long long>(dwFileStart) + sizeof(BITMAPFILEHEADER);
        if (coreHeaderPosition > static_cast<unsigned long long>((std::numeric_limits<long>::max)()) ||
			!pStream->SetPosition(static_cast<long>(coreHeaderPosition), SEEK_SET)) {
			TRACE ("Failed to back up header");
			goto $abort;
		}
        BITMAPCOREHEADER BmpCoreHdr;
        iBytes = pStream->Read (&BmpCoreHdr, sizeof(BmpCoreHdr)); 
        if (iBytes != sizeof(BmpCoreHdr)) {
            TRACE("Failed to read BITMAPCOREHEADER");
            goto $abort;
        }

        BmpInfoHdr.biSize = sizeof(BITMAPINFOHEADER);
        BmpInfoHdr.biWidth = (int) BmpCoreHdr.bcWidth;
        BmpInfoHdr.biHeight = (int) BmpCoreHdr.bcHeight;
        BmpInfoHdr.biPlanes = BmpCoreHdr.bcPlanes;
        BmpInfoHdr.biBitCount = BmpCoreHdr.bcBitCount;
        BmpInfoHdr.biCompression = BI_RGB;
        BmpInfoHdr.biSizeImage = 0;
        BmpInfoHdr.biXPelsPerMeter = 0;
        BmpInfoHdr.biYPelsPerMeter = 0;
        BmpInfoHdr.biClrUsed = 0;
        BmpInfoHdr.biClrImportant = 0;
    }

	// Sanity checks for hostile or corrupt bitmap geometry. These dimensions
	// keep every subsequent legacy stride calculation in a bounded int range.
	if (!IsValidAvatarBitmap(BmpInfoHdr)) {
		TRACE ("Bitmap bad - returning failure");
		goto $abort;
	}

    // Work out how much memory we need for the BITMAPINFO
    // structure, color table and then for the bits.  
    // Allocate the memory blocks.
    // Copy the BmpInfoHdr we have so far,
    // and then read in the color table from the file.
    int iColors;
    int iColorTableSize;
    iColors = NumDIBColorEntries ((LPBITMAPINFO) &BmpInfoHdr);
	if (iColors < 0 || iColors > MAX_PALETTE_SIZE) {
		TRACE("Invalid bitmap palette size");
		goto $abort;
	}
    iColorTableSize = iColors * sizeof(RGBQUAD);
	UINT iBitsSize;
    int iBISize;
    iBISize = sizeof(BITMAPINFOHEADER) + iColorTableSize;
	if (BmpFileHdr.bfSize < BmpFileHdr.bfOffBits) {
		TRACE("Bitmap bit offset exceeds file size");
		goto $abort;
	}
	dwBitsSize = BmpFileHdr.bfSize - BmpFileHdr.bfOffBits;
	if (dwBitsSize == 0 || dwBitsSize > MAX_AVATAR_IMAGE_BYTES) {
		TRACE("Bitmap data exceeds its security bound");
		goto $abort;
	}
	iBitsSize = static_cast<UINT>(dwBitsSize);
	bitmapHeight = BmpInfoHdr.biHeight < 0 ? -BmpInfoHdr.biHeight : BmpInfoHdr.biHeight;
	decodedSize = static_cast<unsigned long long>(DIBStorageWidth(BmpInfoHdr.biWidth, BmpInfoHdr.biBitCount)) *
		static_cast<unsigned long long>(bitmapHeight);
	if (decodedSize == 0 || decodedSize > MAX_AVATAR_IMAGE_BYTES ||
		((BmpInfoHdr.biCompression == BI_RGB || BmpInfoHdr.biCompression == BI_BITFIELDS) &&
		 static_cast<unsigned long long>(iBitsSize) < decodedSize)) {
		TRACE("Bitmap geometry exceeds the supplied bit data");
		goto $abort;
	}

    // Allocate the memory for the header.
    pBmpInfo = (LPBITMAPINFO)malloc (iBISize);
    if (!pBmpInfo) {
        TRACE("Out of memory for DIB header");
        goto $abort;
    }

    // Copy the header we already have.
    memcpy (pBmpInfo, &BmpInfoHdr, sizeof(BITMAPINFOHEADER));

	if (iColorTableSize > 0) {
		// Now read the color table from the file.
		if (bIsPM == FALSE) {
			// Read the color table from the file.
			iBytes = pStream->Read (((LPBYTE) pBmpInfo) + sizeof(BITMAPINFOHEADER),
								 iColorTableSize);
			if (iBytes != iColorTableSize) {
				TRACE("Failed to read color table");
				goto $abort;
			}
		} else {
			// Read each PM color table entry in turn and convert it
			// to Win DIB format as we go.
			LPRGBQUAD lpRGB;
			lpRGB = (LPRGBQUAD) ((LPBYTE) pBmpInfo + sizeof(BITMAPINFOHEADER));
			int i;
			RGBTRIPLE rgbt;
			for (i = 0; i < iColors; i++) {
				iBytes = pStream->Read (&rgbt, sizeof(RGBTRIPLE));
				if (iBytes != sizeof(RGBTRIPLE)) {
					TRACE("Failed to read RGBTRIPLE");
					goto $abort;
				}
				lpRGB->rgbBlue = rgbt.rgbtBlue;
				lpRGB->rgbGreen = rgbt.rgbtGreen;
				lpRGB->rgbRed = rgbt.rgbtRed;
				lpRGB->rgbReserved = 0;
				lpRGB++;
			}
		}
	}

    // Allocate the memory for the bits
    // and read the bits from the file.
    pBits = (BYTE*) malloc (iBitsSize);
    if (!pBits) {
        TRACE("Out of memory for DIB bits");
        goto $abort;
    }
   #ifdef AVATAR_NOT_CLIENT
	ZeroMemory (pBits, iBitsSize);
   #endif

    // Seek to the bits in the file.
	bitsPosition = static_cast<unsigned long long>(dwFileStart) + BmpFileHdr.bfOffBits;
	if (bitsPosition > static_cast<unsigned long long>((std::numeric_limits<long>::max)()) ||
		!pStream->SetPosition(static_cast<long>(bitsPosition), SEEK_SET)) {
		TRACE ("Failed to seek to bit data.");
		goto $abort;
	}

    // Read the bits.
    iBytes = pStream->Read (pBits, iBitsSize);
    if (iBytes != iBitsSize) {
        TRACE("Failed to read bits");
        goto $abort;
    }

    // Everything went OK.
	if (pBmpInfo->bmiHeader.biCompression == BI_RLE4 ||
		pBmpInfo->bmiHeader.biCompression == BI_RLE8)
		pBmpInfo->bmiHeader.biSizeImage = iBitsSize;
    if (m_pBMI != NULL) 
		free (m_pBMI);
    m_pBMI = pBmpInfo; 
    if (m_bMyBits && (m_pBits != NULL)) 
		free(m_pBits);
    m_pBits = pBits;
    m_bMyBits = TRUE;
	// djk -- this shouldn't be necessary, but there's a bug in windows that mandates drawing
	//        be confined to non-rle bitmaps
	if (!ConvertToNonRLE())
		return FALSE;
    return TRUE;
                
$abort: // Something went wrong.
    if (pBmpInfo) 
		free (pBmpInfo);
    if (pBits) 
		free (pBits);
    return FALSE;
}

// This function is like Create, but does take ownership of the bits, and supports
// DIBs other than 256-color.

BOOL 
CAvatarDIB::Create(
BITMAPINFO* pBMI, 
BYTE* pBits)
{
    ASSERT(pBMI);
    ASSERT(pBits);
	if (pBMI == NULL || pBits == NULL || !IsValidAvatarBitmap(pBMI->bmiHeader) ||
		pBMI->bmiHeader.biSize < sizeof(BITMAPINFOHEADER) ||
		pBMI->bmiHeader.biSize > sizeof(BITMAPINFOHEADER) * 6)
		return FALSE;

	// Allocate enough room for the BITMAPINFO and copy it.
	int iColors = NumDIBColorEntries ((LPBITMAPINFO)pBMI);
	if (iColors < 0 || iColors > MAX_PALETTE_SIZE) {
		return FALSE;
	}
	const int iColorTableSize = iColors * sizeof(RGBQUAD);
	BITMAPINFO* pNewBMI = static_cast<BITMAPINFO*>(malloc(pBMI->bmiHeader.biSize + iColorTableSize));
	if (!pNewBMI) {
		return FALSE;
	}
	memcpy(pNewBMI, pBMI, pBMI->bmiHeader.biSize + iColorTableSize);

	// Use the bits passed in by the caller.
	if (m_pBMI != NULL)
		free(m_pBMI);
	m_pBMI = pNewBMI;
    if (m_bMyBits && (m_pBits != NULL)) 
		free(m_pBits);
    m_pBits = pBits;
	m_bMyBits = TRUE;
	return TRUE;
}

// ============================================================================
// CAvatarFileImage implementation

#if defined(AVATAR_READ)

// Sets the proper position in the stream to read from.

BOOL 
CAvatarFileImage::SetProperPosition(
CAvatarStream * pStream)
{
	ASSERT (m_pImage != NULL);
	if (m_pImage->m_dwStreamOffset != (DWORD)-1L) {
		if (m_pImage->m_dwStreamOffset > static_cast<DWORD>((std::numeric_limits<long>::max)()) ||
			!pStream->SetPosition(static_cast<long>(m_pImage->m_dwStreamOffset), SEEK_SET)) {
			TRACE("Seek failed");
			return FALSE;
		}
	}
	return TRUE;
}

// Based on the image's palette type, gets the proper palette.

BOOL
CAvatarFileImage::GetProperPalette(
CAvatarStream * pStream,
CAvatarPalette * pPalette)
{
	BOOL bLoadPalette = FALSE;
	const COLORREF * pclrrefUse = NULL;
	int nClrs = 0;

	ASSERT (m_pImage != NULL);
	switch (m_pImage->m_byPaletteType) {
		case AIP_NOPALETTE:
			break;
		case AIP_GLOBALPALETTE:
			// Copy entries from the global palette.
			pclrrefUse = m_pImage->m_pGlobalPalette->m_pclrref;
			nClrs = m_pImage->m_pGlobalPalette->m_nColorCount;
			break;
		case AIP_LOCALPALETTE: {
			// The local palette is stored inline in the stream. Verify that there
			// is a palette here in the stream, as indicated by an AK_COLORPALETTE
			// tag, and a size value.
			AVBINT16 n[2];
			if (pStream->Read (n, sizeof(n)) != sizeof(n) || n[0] != AK_COLORPALETTE) {
				TRACE("No palette here!");
				return FALSE;
			}
			bLoadPalette = TRUE;
			break;
		}
		case AIP_MONOCHROME:
			// Use the monochrome palette.
			pclrrefUse = MonochromePalette;
			nClrs = 2;
			break;
		case AIP_MASKEDMONO:
		case AIP_DUALMASK:
			// Use a 2-bit masked monochrome palette.
			pclrrefUse = MaskedMonoPalette;
			nClrs = 4;
			break;
	}

	if (bLoadPalette) {
		return pPalette->Read (pStream);
	}
	else if (pclrrefUse != NULL) {
		return pPalette->SetFrom (pclrrefUse, nClrs);
	}
	else {
		return TRUE;
	}
}

#endif // AVATAR_READ

#if defined(AVATAR_READ)

// ============================================================================
// CAvatarFileDIBImage implementation

// Read an image from the stream.

BOOL
CAvatarFileDIBImage::Read(
CAvatarStream * pStream)
{
	ASSERT(m_pImage->m_pDib == NULL);
	ASSERT(m_pImage->m_byPaletteType == AIP_NOPALETTE);

	TRY
	{
		m_pImage->m_pDib = new CAvatarDIB;
	}
	CATCH_ALL(e)
	{
		TRACE("Failed to allocate DIB");
		return FALSE;
	}
	END_CATCH_ALL

	if (!SetProperPosition (pStream)) {
		return FALSE;
	}

	if (!m_pImage->m_pDib->Load (pStream))
	{
		TRACE("Failed to load DIB");
		delete m_pImage->m_pDib;
		m_pImage->m_pDib = NULL;
		return FALSE;
	}
	return TRUE;
}

#endif // AVATAR_READ

// ============================================================================
// CAvatarFileZlibImage implementation

// Read an image from the stream.

#if defined(AVATAR_READ)

BOOL
CAvatarFileZlibImage::Read(
CAvatarStream * pStream)
{
	ASSERT(m_pImage->m_pDib == NULL);
	LPBITMAPINFO lpbmi = NULL;
	LPBYTE pbBitmapData = NULL;
	UINT cbBitmapData = 0;
	long imageHeight = 0;
	unsigned long long expectedBitmapSize = 0;

	// Set proper position in file.
	if (!SetProperPosition (pStream)) {
		return FALSE;
	}

	ASSERT (m_pImage->m_byPaletteType != AIP_NOPALETTE);

	// Get the correct palette.
	CAvatarPalette pal;
	if (!GetProperPalette (pStream, &pal)) {
		TRACE("Failed to get palette");
		return FALSE;
	}

	TRY
	{
		m_pImage->m_pDib = new CAvatarDIB;
	}
	CATCH_ALL(e)
	{
		TRACE("Failed to allocate DIB");
		return FALSE;
	}
	END_CATCH_ALL

	// Read the size of the infoheader, and do some sanity checking.
	AVBINT32 dwHeaderSize;
	if (!pStream->Read32 (&dwHeaderSize)) {
		goto $abort;
	}
	if (dwHeaderSize < sizeof(BITMAPINFOHEADER) || dwHeaderSize > sizeof(BITMAPINFOHEADER) * 6) {
		TRACE("Apparent bad bitmap info header, returning failure.");
		goto $abort;
	}

	// Allocate room for a BITMAPINFO with enough color entries.
	lpbmi = (LPBITMAPINFO)malloc (dwHeaderSize + pal.m_nColorCount * sizeof(RGBQUAD));
	if (lpbmi == NULL) {
		TRACE("Failed to allocate BITMAPINFO structure");
		goto $abort;
	}
	lpbmi->bmiHeader.biSize = dwHeaderSize;

	// Read the BITMAPINFOHEADER, starting from the second field.
	if (pStream->Read (((LPBYTE)lpbmi) + sizeof(DWORD), dwHeaderSize - sizeof(DWORD)) != 
			dwHeaderSize - sizeof(DWORD)) {
		TRACE("Failed to read BITMAPINFOHEADER");
		goto $abort;
	}

	// Do some sanity checking to make sure the bitmap hasn't been hacked.
	if (!IsValidAvatarBitmap(lpbmi->bmiHeader)) {
		TRACE("Invalid image, returning failure");
		goto $abort;
	}

	// Copy the palette's color table into the info structure.
	if (pal.m_nColorCount > 0) {
		RGBQUAD* pColors = (RGBQUAD*)(((LPBYTE)lpbmi) + dwHeaderSize);
		for (int iColor = pal.m_nColorCount - 1; iColor >= 0; iColor--)
		{
			SET_RGBQUAD_FROM_COLORREF (pColors + iColor, pal.m_pclrref[iColor]);
		};
	}

	// Read the bitmap data. Don't just check the return value, but also check that
	// the values make some sense.
	if (!pStream->AllocAndReadCompressedBuffer ((PVOID *)&pbBitmapData, &cbBitmapData)) {
		goto $abort;
	}

	// Do things match?

	imageHeight = lpbmi->bmiHeader.biHeight < 0
		? -lpbmi->bmiHeader.biHeight : lpbmi->bmiHeader.biHeight;
	expectedBitmapSize =
		static_cast<unsigned long long>(DIBStorageWidth(lpbmi->bmiHeader.biWidth, lpbmi->bmiHeader.biBitCount)) *
		static_cast<unsigned long long>(imageHeight);
	if (expectedBitmapSize == 0 || expectedBitmapSize > MAX_AVATAR_IMAGE_BYTES ||
		expectedBitmapSize != cbBitmapData)
	{
		TRACE("Image size mismatch");
		goto $abort;
	}

	// Assign the bits to the DIB.
	if (m_pImage->m_pDib->Create (lpbmi, pbBitmapData)) {
		free (lpbmi);
		return TRUE;
	}

   $abort:
	// Failure
	free (pbBitmapData);
    free (lpbmi);
	delete m_pImage->m_pDib;
	m_pImage->m_pDib = NULL;
	return FALSE;
}

#endif // AVATAR_READ

// ============================================================================
// CAvatarX implementation


#if defined(AVATAR_READ)

// Loads and returns an Avatar from an open stream. This is a static function,
// use it to create Avatars.


CAvatarX* 
CAvatarX::LoadAvatar(
CAvatarStream * pStream)
{
	long nResourcesAdjustment = 0;

	if (!pStream->Open ()) {
		return FALSE;
	}

	// Get the header.
	AVATARHEADER avh;
	if (pStream->Read (&avh, sizeof(avh)) != sizeof(avh)) {
		pStream->Close ();
		return NULL;
	}

	// Verify "magic number" - can have the old or new one.
	if (avh.nMagicNum != AF_MAGICNUM && avh.nMagicNum != AF_MAGICNUM_NEW) {
		TRACE("Not an avatar file");
		pStream->Close ();
		return NULL;
	}

	// Create the right type of avatar.
	CAvatarX * pAvatar = NULL;
	TRY
	{
		switch (avh.nType) {
			case AT_COMPLEX:
				pAvatar = new CAvatarComplex;
				break;
			case AT_SIMPLE:
				pAvatar = new CAvatarSimple;
				break;
			default:
				TRACE("Invalid avatar type");
				goto $abort;
		}
	}
	CATCH_ALL(e)
	{
		TRACE("Could not allocate avatar");
		goto $abort;
	}
	END_CATCH_ALL

	// Check version number. We should be able to load any version with the same
	// major version number.
	if (HIWORD(avh.nVersion) != 0) {
		TRACE("Unsupported version number");
		goto $abort;
	}

	// Go through, reading each tag.
	AVBINT16 tag;
	AVBINT16 size;
	while (TRUE) {

		if (!pStream->Read16 (&tag)) {
			goto $abort;
		}
		// Newer tags are stored with a size.
		if (tag >= AK_ICON_NEW && !pStream->Read16 (&size)) {
			goto $abort;
		}

		if (tag == AK_STARTDATA) {
			// That's it, we're done.
			break;
		}

		// Let the avatar handle the tag. If it can't handle it,
		// and it's a new tag, it will skip it.
		if (!pAvatar->HandleLoadTag (pStream, tag, size, nResourcesAdjustment)) {
			goto $abort;
		}
	}

	pStream->Close ();
	// Compact the pose array.
	pAvatar->m_arrPoses.FreeExtra ();
	return pAvatar;

   $abort:
	pStream->Close ();
	if (pAvatar != NULL) {
		delete pAvatar;
	}
	return NULL;
}

// Utility function to read in a string from the given stream, and put it
// in the given string pointer.

#define MAX_ANY_STRING	1024

BOOL
ReadAvFileString(
CAvatarStream * pStream,
LPTSTR *		ppszString,
int				nMaxLength)
{
	ASSERT(nMaxLength <= MAX_ANY_STRING);
	TCHAR szString[MAX_ANY_STRING];
	if (!pStream->ReadString (szString, nMaxLength)) {
		return FALSE;
	}
	*ppszString = strdup (szString);
	return *ppszString != NULL;
}

#define MAX_AVATAR_NAME	60
#define MAX_URL			512
#define MAX_COPYRIGHT	256

// Handles a single load tag. Fails if it can't handle it or skip it.
// Derived classes can override this function, but should call this base
// implementation to handle all tags common to all avatar types.

BOOL
CAvatarX::HandleLoadTag(
CAvatarStream * pStream,
AVBINT16			tag,
AVBINT16			size,
long &			nResourcesAdjustment)
{
	BOOL bRet;

	switch (tag) {
		case AK_NAME:
			bRet = ReadAvFileString (pStream, &m_name, MAX_AVATAR_NAME);
			break;
		
		case AK_ORIGINAL_URL:
			bRet = ReadAvFileString (pStream, &m_pszOriginalURL, MAX_URL);
			break;

		case AK_OVERRIDE_URL:
			bRet = ReadAvFileString (pStream, &m_pszNewURL, MAX_URL);
			break;

		case AK_COPYRIGHT:
			bRet = ReadAvFileString (pStream, &m_pszCopyright, MAX_COPYRIGHT);
			break;

	   #if defined(AVATAR_NOT_CLIENT)
		case AK_USAGE_FLAGS:
			bRet = pStream->Read8 (&m_byUsageFlags);
			break;
	   #endif
	   
		case AK_STYLE:
		{
			AVBINT16 style;
			bRet = pStream->Read16 (&style);
			if (bRet) {
				m_style = (UCHAR)style;
			}
			break;
		}
		
		case AK_FLAGS:
		{
			AVBINT16 flags;
			bRet = pStream->Read16 (&flags);
			if (bRet) {
				m_flags = (UCHAR)flags;
			}
			break;
		}
		
		case AK_ICON:
		case AK_ICON_NEW:
		{
			AVATARICONDATA icondata;

			// The old tag refers to a DIB image.
			if (tag == AK_ICON) {
				if (!pStream->Read32 (&icondata.dwOffset)) {
					return FALSE;
				}
				icondata.byFormat = AIF_DIB;
				icondata.byPalette = AIP_NOPALETTE;
			} else {
				if (pStream->Read (&icondata, sizeof(icondata)) != sizeof(icondata)) {
					return FALSE;
				}
			}

			// Create the pose.
			if (!AdjustAvatarOffset(icondata.dwOffset, nResourcesAdjustment))
				return FALSE;
			USHORT nPose = CreatePose (pStream, icondata.dwOffset, 
								icondata.byFormat, icondata.byPalette);
			m_icon = nPose;
			bRet = (nPose != INVALID_POSE_ID);
			break;
		}

		case AK_COLORPALETTE:
			// Ye olde global palette.
			bRet = m_palette.Read (pStream);
			break;

		case AK_OFFSET_ADJUSTMENT:
		{
			// All offsets following this record are adjusted by the given
			// signed 2-bit integer amount. This record can be used by
			// tools which do not know the file format to add or remove things 
			// in a file.
			int nValue;
			bRet = pStream->Read32 ((AVBINT32 *)&nValue);
			if (bRet) {
				if (!AddResourceAdjustment(nResourcesAdjustment, nValue))
					return FALSE;
			}
			break;
		}

		default:
			// Strange tag - if it's a new one we can skip it.
			if (tag >= AK_ICON_NEW) {
				TRACE ("Unrecognized tag - skipped");
				bRet = pStream->SetPosition (size, SEEK_CUR);
			}
			else {
				// Can't skip this tag, and we're basically hosed.
				TRACE ("Unrecognized tag - can't skip, aborting");
				bRet = FALSE;
			}
			break;
	}

	return bRet;
}

// Adds a pose to the pose table, and returns the ID for it. This pose 
// is not expected to have a mask (unless the image's palette type is
// AIP_MASKEDMONO). This function is a wrapper for CreatePoseWithMask
// function below.

USHORT
CAvatarX::CreatePose(
CAvatarStream * pStream,
DWORD dwOffset,
BYTE  byFormat,
BYTE  byPaletteType)
{
	DWORD dwOffsets[3];
	BYTE  byFormats[3];
	BYTE  byPaletteTypes[3];
	dwOffsets[0] = dwOffset;
	dwOffsets[1] = dwOffsets[2] = 0;
	byFormats[0] = byFormat;
	byFormats[1] = byFormats[2] = 0;
	byPaletteTypes[0] = byPaletteType;
	byPaletteTypes[1] = byPaletteTypes[2] = 0;
	return CreatePoseWithMask (pStream, dwOffsets, byFormats, byPaletteTypes);
}

// Adds a pose to the pose table, and returns the ID for it. This pose 
// could either have a mask and aura entries, or the main image could be
// of AIP_MASKEDMONO format. If the latter is true, the mask and aura
// entries are ignored.

USHORT
CAvatarX::CreatePoseWithMask(
CAvatarStream * pStream,
LPDWORD pdwOffsets,
LPBYTE pbyFormats,
LPBYTE pbyPaletteTypes)
{
	CPose * pPose = NULL;
	int nPosition;

	TRY
	{
		pPose = new CPose (pdwOffsets, pbyFormats, pbyPaletteTypes);
		nPosition = m_arrPoses.Add (pPose);
	}
	CATCH_ALL(e)
	{
		if (pPose != NULL) {
			delete pPose;
		}
		return INVALID_POSE_ID;
	}
	END_CATCH_ALL
	return nPosition + 1;
}

// Handles a single load tag. If it can't, it passes it on to the parent class.

BOOL
CAvatarSimple::HandleLoadTag(
CAvatarStream * pStream,
AVBINT16			tag,
AVBINT16			size,
long &			nResourcesAdjustment)
{
	BOOL bRet;

	switch (tag) {
		case AK_NBODIES:
		case AK_NBODIES2:
			bRet = LoadBodyRecs (pStream, tag == AK_NBODIES, nResourcesAdjustment);
			break;

		default:
			// Let the base class handle it.
			bRet = CAvatarX::HandleLoadTag (pStream, tag, size, nResourcesAdjustment);
			break;
	}
	
	return bRet;
}

// Loads all the bodies in the body record.

BOOL
CAvatarSimple::LoadBodyRecs(
CAvatarStream * pStream,
BOOL			bOldTag,	// The old tag has more padding bytes.
long &			nResourcesAdjustment)
{
	AVBINT16 nCount;
	if (!pStream->Read16 (&nCount)) {
		return FALSE;
	}
	if (nCount == 0 || nCount > MAX_AVATAR_COMPONENTS ||
		nCount > static_cast<AVBINT16>((std::numeric_limits<short>::max)())) {
		TRACE("Invalid simple-avatar body count");
		return FALSE;
	}

	bRec = (RBODYREC *)malloc (sizeof (RBODYREC) * nCount);
	if (bRec == NULL) {
		return FALSE;
	}

	m_nBodies = nCount;

	// Go through and read each record. The size of each record depends on 
	// whether this is an old or new tag.
	AVATARBODYDATA bodydata;
	UINT nSizeRead = bOldTag ? sizeof(bodydata.olddata) : sizeof(bodydata.newdata);
	BOOL bRet = TRUE;
	UINT i;
	AVBINT32 dwPrevImageOffset = 0;
	for (i = 0; i < nCount && bRet; i++) {

		if (pStream->Read (&bodydata, nSizeRead) != nSizeRead) {
			bRet = FALSE;
			break;
		}

		if (bodydata.newdata.dwImageOffset != dwPrevImageOffset) {
			if (!AdjustAvatarOffset(bodydata.newdata.dwImageOffset, nResourcesAdjustment) ||
				!AdjustAvatarOffset(bodydata.newdata.dwMaskOffset, nResourcesAdjustment) ||
				!AdjustAvatarOffset(bodydata.newdata.dwAuraOffset, nResourcesAdjustment)) {
				bRet = FALSE;
				break;
			}
			bRec[i].poseID = CreatePoseWithMask (pStream, 
									&bodydata.newdata.dwImageOffset, 
									&bodydata.newdata.byImageFormat,
									&bodydata.newdata.byImagePaletteType);
			if (bRec[i].poseID == INVALID_POSE_ID) {
				bRet = FALSE;
				break;
			}
			dwPrevImageOffset = bodydata.newdata.dwImageOffset;
		} else {
			// This is the ditto case.
			bRec[i].poseID = bRec[i - 1].poseID;
		}

	   #if defined(AVATAR_NOT_CLIENT)
	    bRec[i].emotion = bodydata.newdata.nEmotion;
	   #else // AVATAR_NOT_CLIENT
		bRec[i].emotion = EmotionToFloat(bodydata.newdata.nEmotion);
	   #endif // !AVATAR_NOT_CLIENT
		bRec[i].intensity = bodydata.newdata.byIntensity / (float)255;
		bRec[i].faceX = (UCHAR)bodydata.newdata.x;
		bRec[i].faceY = (UCHAR)bodydata.newdata.y;
	}

	if (!bRet) {
		free (bRec);
		bRec = NULL;
		m_nBodies = 0;
	}
	return bRet;
}

// ============================================================================
// CAvatarComplex implementation

// Handles a single load tag. If it can't, it passes it on to the parent class.

BOOL
CAvatarComplex::HandleLoadTag(
CAvatarStream * pStream,
AVBINT16			tag,
AVBINT16			size,
long &			nResourcesAdjustment)
{
	BOOL bRet;

	switch (tag) {
		case AK_NFACES:
		case AK_NFACES2:
			bRet = LoadFaceRecs (pStream, tag == AK_NFACES, nResourcesAdjustment);
			break;

		case AK_NTORSOS:
		case AK_NTORSOS2:
			bRet = LoadTorsoRecs (pStream, tag == AK_NTORSOS, nResourcesAdjustment);
			break;

		default:
			// Let the base class handle it.
			bRet = CAvatarX::HandleLoadTag (pStream, tag, size, nResourcesAdjustment);
			break;
	}
	
	return bRet;
}

// Loads all the faces in the face record.

BOOL
CAvatarComplex::LoadFaceRecs(
CAvatarStream * pStream,
BOOL			bOldTag,	// The old tag has more padding bytes.
long &			nResourcesAdjustment)
{
	AVBINT16 nCount;
	if (!pStream->Read16 (&nCount)) {
		return FALSE;
	}
	if (nCount == 0 || nCount > MAX_AVATAR_COMPONENTS ||
		nCount > static_cast<AVBINT16>((std::numeric_limits<short>::max)())) {
		TRACE("Invalid complex-avatar face count");
		return FALSE;
	}

	fRec = (FACEREC *)malloc (sizeof (FACEREC) * nCount);
	if (fRec == NULL) {
		return FALSE;
	}

	nFaces = nCount;

	// Go through and read each record. The size of each record depends on 
	// whether this is an old or new tag.
	AVATARFACEDATA facedata;
	UINT nSizeRead = bOldTag ? sizeof(facedata.olddata) : sizeof(facedata.newdata);
	BOOL bRet = TRUE;
	UINT i;
	AVBINT32 dwPrevImageOffset = 0;
	for (i = 0; i < nCount && bRet; i++) {

		if (pStream->Read (&facedata, nSizeRead) != nSizeRead) {
			bRet = FALSE;
			break;
		}

		if (facedata.newdata.dwImageOffset != dwPrevImageOffset) {
			if (!AdjustAvatarOffset(facedata.newdata.dwImageOffset, nResourcesAdjustment) ||
				!AdjustAvatarOffset(facedata.newdata.dwMaskOffset, nResourcesAdjustment) ||
				!AdjustAvatarOffset(facedata.newdata.dwAuraOffset, nResourcesAdjustment)) {
				bRet = FALSE;
				break;
			}
			fRec[i].poseID = CreatePoseWithMask (pStream, 
									&facedata.newdata.dwImageOffset, 
									&facedata.newdata.byImageFormat,
									&facedata.newdata.byImagePaletteType);
			if (fRec[i].poseID == INVALID_POSE_ID) {
				bRet = FALSE;
				break;
			}
			dwPrevImageOffset = facedata.newdata.dwImageOffset;
		} else {
			// This is the ditto case.
			fRec[i].poseID = fRec[i - 1].poseID;
		}

	   #if defined(AVATAR_NOT_CLIENT)
	    fRec[i].emotion = facedata.newdata.nEmotion;
	   #else // AVATAR_NOT_CLIENT
		fRec[i].emotion = EmotionToFloat(facedata.newdata.nEmotion);
	   #endif // !AVATAR_NOT_CLIENT
		fRec[i].intensity = facedata.newdata.byIntensity / (float)255;
		fRec[i].xCX = facedata.newdata.cx;
		fRec[i].yCX = facedata.newdata.cy;
		fRec[i].delta_xCX = facedata.newdata.cxDelta;
		fRec[i].delta_yCX = facedata.newdata.cyDelta;
		fRec[i].faceX = (UCHAR)facedata.newdata.x;
		fRec[i].faceY = (UCHAR)facedata.newdata.y;
	}

	if (!bRet) {
		free (fRec);
		fRec = NULL;
		nFaces = 0;
	}
	return bRet;
}

// Loads all the torsos in the torso record.

BOOL
CAvatarComplex::LoadTorsoRecs(
CAvatarStream * pStream,
BOOL			bOldTag,	// The old tag has more padding bytes.
long &			nResourcesAdjustment)
{
	AVBINT16 nCount;
	if (!pStream->Read16 (&nCount)) {
		return FALSE;
	}
	if (nCount == 0 || nCount > MAX_AVATAR_COMPONENTS ||
		nCount > static_cast<AVBINT16>((std::numeric_limits<short>::max)())) {
		TRACE("Invalid complex-avatar torso count");
		return FALSE;
	}

	bRec = (BODYREC *)malloc (sizeof (BODYREC) * nCount);
	if (bRec == NULL) {
		return FALSE;
	}

	nTorsos = nCount;

	// Go through and read each record. The size of each record depends on 
	// whether this is an old or new tag.
	AVATARTORSODATA torsodata;
	UINT nSizeRead = bOldTag ? sizeof(torsodata.olddata) : sizeof(torsodata.newdata);
	BOOL bRet = TRUE;
	UINT i;
	AVBINT32 dwPrevImageOffset = 0;
	for (i = 0; i < nCount && bRet; i++) {

		if (pStream->Read (&torsodata, nSizeRead) != nSizeRead) {
			bRet = FALSE;
			break;
		}

		if (torsodata.newdata.dwImageOffset != dwPrevImageOffset) {
			if (!AdjustAvatarOffset(torsodata.newdata.dwImageOffset, nResourcesAdjustment) ||
				!AdjustAvatarOffset(torsodata.newdata.dwMaskOffset, nResourcesAdjustment) ||
				!AdjustAvatarOffset(torsodata.newdata.dwAuraOffset, nResourcesAdjustment)) {
				bRet = FALSE;
				break;
			}
			bRec[i].poseID = CreatePoseWithMask (pStream, 
									&torsodata.newdata.dwImageOffset, 
									&torsodata.newdata.byImageFormat,
									&torsodata.newdata.byImagePaletteType);
			if (bRec[i].poseID == INVALID_POSE_ID) {
				bRet = FALSE;
				break;
			}
			dwPrevImageOffset = torsodata.newdata.dwImageOffset;
		} else {
			// This is the ditto case.
			bRec[i].poseID = bRec[i - 1].poseID;
		}

	   #if defined(AVATAR_NOT_CLIENT)
	    bRec[i].emotion = torsodata.newdata.nEmotion;
	   #else // AVATAR_NOT_CLIENT
		bRec[i].emotion = EmotionToFloat(torsodata.newdata.nEmotion);
	   #endif // !AVATAR_NOT_CLIENT
		bRec[i].intensity = torsodata.newdata.byIntensity / (float)255;
		bRec[i].xCX = torsodata.newdata.cx;
		bRec[i].yCX = torsodata.newdata.cy;
	}

	if (!bRet) {
		free (bRec);
		bRec = NULL;
		nTorsos = 0;
	}
	return bRet;
}

#endif // AVATAR_READ

#if defined(AVATARFILER)

// Adds a pose to the pose table.

USHORT
CAvatarX::CreatePose(
LPCTSTR pszFile,
LPCTSTR pszFileMask /*= NULL*/,
LPCTSTR pszFileAura /*= NULL*/)
{
	CPose * pPose = NULL;
	int nPosition;

	TRY
	{
		LPCTSTR psz[3];
		psz[0] = pszFile;
		psz[1] = pszFileMask;
		psz[2] = pszFileAura;
		pPose = new CPose (psz);
		nPosition = m_arrPoses.Add (pPose);
	}
	CATCH_ALL(e)
	{
		if (pPose != NULL) {
			delete pPose;
		}
		return INVALID_POSE_ID;
	}
	END_CATCH_ALL
	return nPosition + 1;
}

#endif // AVATARFILER

// ============================================================================
// CPose implementation

// Load a single pose, including all three masks. There are two versions of this,
// the AvatarFiler version, which only needs to load some DIBs and do some fixing
// up, and the normal version.

#if defined(AVATARFILER)

BOOL
CPose::Load(
CAvatarStream * pStream,
CAvatarPalette * pGlobalPalette)
{
	TRY
	{
		for (int i = 0; i < 3; i++) {
			if (!m_strFiles[i].IsEmpty ()) {
				CAvatarFileStream stream (m_strFiles[i]);
				if (!stream.Open ()) {
					return FALSE;
				}
				m_pdibs[i] = new CAvatarDIB;
				if (!m_pdibs[i]->Load (&stream)) {
					return FALSE;
				}
			}
		}
	}
	CATCH_ALL(e)
	{

		return FALSE;
	}
	END_CATCH_ALL

	// Do some work based on images available, so they save optimized.

	m_byFormats[0] = AIF_LZDEFLATE;
	m_byFormats[1] = AIF_LZDEFLATE;
	m_byFormats[2] = AIF_LZDEFLATE;
	m_byPaletteTypes[0] = AIP_LOCALPALETTE;
	m_byPaletteTypes[1] = AIP_MONOCHROME;
	m_byPaletteTypes[2] = AIP_MONOCHROME;
	if (m_pdibs[0] != NULL && m_pdibs[0]->GetBitmapInfoAddress ()->bmiHeader.biBitCount == 1) {
		m_byPaletteTypes[0] = AIP_MASKEDMONO;
	}
	else if (m_pdibs[1] != NULL) {
		m_byPaletteTypes[1] = AIP_DUALMASK;
	}
	return TRUE;
}

#else // AVATARFILER

BOOL
CPose::Load(
CAvatarStream * pStream,
CAvatarPalette * pGlobalPalette)
{
	// Already loaded?
	if (m_pdibs[0] != NULL) {
		return TRUE;
	}

	if (!pStream->Open ()) {
		return FALSE;
	}

	AVATARIMAGE im;
	for (int i = 0; i < 3; i++) {
		if (m_dwOffsets[i] != 0) {
			// Create an AVATARIMAGE struct, and load the image.
			im.m_dwStreamOffset = m_dwOffsets[i];
			im.m_byFormat = m_byFormats[i];
			im.m_byPaletteType = m_byPaletteTypes[i];
			im.m_pGlobalPalette = pGlobalPalette;
			im.m_pDib = NULL;
			BOOL bRet;
			switch (im.m_byFormat) {
				case AIF_DIB:
					bRet = CAvatarFileDIBImage(&im).Read (pStream);
					break;
				case AIF_LZDEFLATE:
					bRet = CAvatarFileZlibImage(&im).Read (pStream);
					break;
				default:
					bRet = FALSE;
					break;
			}
			if (bRet) {
				m_pdibs[i] = im.m_pDib;
			}
			else {
				pStream->Close ();
				return FALSE;
			}
		}
	}
	pStream->Close ();

	// Conversion is required for certain images. 
	//    If Image 0 is a AIP_MASKEDMONO image => need to generate all three images.
	//	  If Image 1 is a AIP_DUALMASK image => need to generate both masks.

	BOOL bRet = TRUE;
	if (m_byPaletteTypes[0] == AIP_MASKEDMONO && m_pdibs[0] != NULL) {
		bRet = ConvertFromMaskedMono (m_pdibs[0]);
	}
	else if (m_byPaletteTypes[1] == AIP_DUALMASK && m_pdibs[1] != NULL) {
		bRet = ConvertFromDualMask (m_pdibs[1]);
	}

	return bRet;
}

#endif // !AVATARFILER

// Lookup tables that help implement fast masked-mono to 1 bit conversion.

// Maps a byte consisting of 4 2-bit pairs into a nybble containing a bit
// from each pair. abcdefgh => aceg or bdfh
// If we chop off the high bit of the byte and look it up, we get the low bits (bdfh).
// If we divide the byte by two and look it up, we get the high bits (aceg).
// To use this table with a 2-byte source value, look up each byte and then combine
// the results by shifting.

static const BYTE byLookupImage[] = { 
	0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3, 
	4, 5, 4, 5, 6, 7, 6, 7, 4, 5, 4, 5, 6, 7, 6, 7, 
	0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3, 
	4, 5, 4, 5, 6, 7, 6, 7, 4, 5, 4, 5, 6, 7, 6, 7, 
	8, 9, 8, 9, 10, 11, 10, 11, 8, 9, 8, 9, 10, 11, 10, 11, 
	12, 13, 12, 13, 14, 15, 14, 15, 12, 13, 12, 13, 14, 15, 14, 15, 
	8, 9, 8, 9, 10, 11, 10, 11, 8, 9, 8, 9, 10, 11, 10, 11, 
	12, 13, 12, 13, 14, 15, 14, 15, 12, 13, 12, 13, 14, 15, 14, 15, 
};

// Entry to take a byte with 4 2-bit pairs, and return a nybble with a 1 for every
// pair that has at least a 1 in it. e.g. 00000000 => 0000, 11111111 => 1111,
// 01001110 => 1011
// To use this table with a 2-byte source value, look up each byte and then combine
// the results by shifting.

static const BYTE byLookupAura[] = { 
	0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3, 
	4, 5, 5, 5, 6, 7, 7, 7, 6, 7, 7, 7, 6, 7, 7, 7, 
	4, 5, 5, 5, 6, 7, 7, 7, 6, 7, 7, 7, 6, 7, 7, 7, 
	4, 5, 5, 5, 6, 7, 7, 7, 6, 7, 7, 7, 6, 7, 7, 7, 
	8, 9, 9, 9, 10, 11, 11, 11, 10, 11, 11, 11, 10, 11, 11, 11, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	8, 9, 9, 9, 10, 11, 11, 11, 10, 11, 11, 11, 10, 11, 11, 11, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	8, 9, 9, 9, 10, 11, 11, 11, 10, 11, 11, 11, 10, 11, 11, 11, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
	12, 13, 13, 13, 14, 15, 15, 15, 14, 15, 15, 15, 14, 15, 15, 15, 
};

// Converts a masked mono (2 bpp) image into three monochrome pictures - an image,
// a mask, and an aura.	Just uses the ConvertMasksCommon function below.

BOOL
CPose::ConvertFromMaskedMono(
CAvatarDIB * pSrcDIB)
{
	CAvatarDIB * pDestDibs[3];
	BOOL bRet = ConvertMasksCommon (pSrcDIB, pDestDibs, 3);
	if (bRet) {
		memcpy (m_pdibs, pDestDibs, sizeof(m_pdibs));
		delete pSrcDIB;
	}
	return bRet;
}

// Converts a dual mask (2 bpp) image into two monochrome masks.
// Just uses the ConvertMasksCommon function below.

BOOL
CPose::ConvertFromDualMask(
CAvatarDIB * pSrcDIB)
{
	CAvatarDIB * pDestDibs[2];
	BOOL bRet = ConvertMasksCommon (pSrcDIB, pDestDibs, 2);
	if (bRet) {
		m_pdibs[1] = pDestDibs[0];
		m_pdibs[2] = pDestDibs[1];
		delete pSrcDIB;
	}
	return bRet;
}

// Converts a 2 bpp image into 2 or 3 bitmaps.

BOOL
CPose::ConvertMasksCommon(
CAvatarDIB * pSrcDIB,
CAvatarDIB * * pDIBsOut, 
int nNumDIBs)
{
	ASSERT (pSrcDIB != NULL);
	ASSERT (pDIBsOut != NULL);
	ASSERT (nNumDIBs == 2 || nNumDIBs == 3);

 	LPBITMAPINFOHEADER pbmihSrc = &pSrcDIB->GetBitmapInfoAddress ()->bmiHeader;
	PBYTE pbDestBits[3];
	PBYTE pbSrcBits = (PBYTE)pSrcDIB->GetBitsAddress ();
	int i;

	MONOBITMAPINFO bmiDest;

	// Set up the BITMAPINFO. All three DIBs will use the same header
	bmiDest.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmiDest.bmiHeader.biWidth = pbmihSrc->biWidth;
	bmiDest.bmiHeader.biHeight = pbmihSrc->biHeight;
	bmiDest.bmiHeader.biPlanes = 1;
	bmiDest.bmiHeader.biBitCount = 1;
	bmiDest.bmiHeader.biCompression = BI_RGB;
	bmiDest.bmiHeader.biSizeImage = 0;
	bmiDest.bmiHeader.biXPelsPerMeter = pbmihSrc->biXPelsPerMeter;
	bmiDest.bmiHeader.biYPelsPerMeter = pbmihSrc->biYPelsPerMeter;
	bmiDest.bmiHeader.biClrUsed = 2;
	bmiDest.bmiHeader.biClrImportant = 2;
	memcpy (bmiDest.bmiColors, MonochromePalette, sizeof(MonochromePalette));

	// In the first pass, just set up each of the 3 DIBs, allocating memory as
	// needed.
	ZeroMemory (pDIBsOut, nNumDIBs * sizeof(CAvatarDIB *));
	UINT nBitsAllocSize = DIBStorageWidth (bmiDest.bmiHeader.biWidth, 1) *
				bmiDest.bmiHeader.biHeight;
	TRY
	{
		for (i = 0; i < nNumDIBs; i++) {
			pbDestBits[i] = (PBYTE)malloc (nBitsAllocSize);
			if (pbDestBits[i] == NULL) {
				AfxThrowMemoryException ();
			}
		   #ifdef AVATAR_NOT_CLIENT
			ZeroMemory (pbDestBits[i], nBitsAllocSize);
		   #endif
			pDIBsOut[i] = new CAvatarDIB;
			if (!pDIBsOut[i]->Create ((LPBITMAPINFO)&bmiDest, pbDestBits[i])) {
				AfxThrowMemoryException ();
			}
		}
	}
	CATCH_ALL(e)
	{
		free (pbDestBits[i]);
		for (i = 0; i < nNumDIBs; i++) {
			if (pDIBsOut[i] != NULL) {
				delete pDIBsOut[i];
			}
		}
		return FALSE;
	}
	END_CATCH_ALL

	// Set up color tables.
	RGBQUAD * pColors;
	pColors = pDIBsOut[0]->GetClrTabAddress ();
	SET_RGBQUAD_FROM_COLORREF (&pColors[0], RGB(255,255,255));
	SET_RGBQUAD_FROM_COLORREF (&pColors[1], RGB(0,0,0));
	pColors = pDIBsOut[1]->GetClrTabAddress ();
	SET_RGBQUAD_FROM_COLORREF (&pColors[0], RGB(255,255,255));
	SET_RGBQUAD_FROM_COLORREF (&pColors[1], RGB(0,0,0));
	if (nNumDIBs == 3) {
		pColors = pDIBsOut[2]->GetClrTabAddress ();
		SET_RGBQUAD_FROM_COLORREF (&pColors[0], RGB(255,255,255));
		SET_RGBQUAD_FROM_COLORREF (&pColors[1], RGB(0,0,0));
	}

	// In the second pass, write out the bits. Since the scan lines are always
	// long aligned, we can just do a simple process of reading two bytes at a 
	// time. The conversion from sixteen bits to eight is done with 
	// lookup tables, for speed.

	int nWordsPerSrcScanLine = pSrcDIB->StorageWidth () / 2;
	int nDestScanLineSize = pDIBsOut[0]->StorageWidth ();
	LPWORD pwSrc = (LPWORD)pbSrcBits;
	int nDestOffset = 0;
	WORD wData;
	for (int y = 0; y < bmiDest.bmiHeader.biHeight; y++) {
		for (int x = 0; x < nWordsPerSrcScanLine; x++) {
			wData = pwSrc[x];

			// The byte for the first image consists of all the even bits of the
			// source word. The byte for the second image consists of all the 
			// odd bits of the source word. If there is a third image, it consists
			// of 1s wherever the corresponding pair in the source word does not
			// equal 00. 
			// 00 => 0,0,0    01 => 1,0,1   10 => 0,1,1     11 => 1,1,1
			// Use the two lookup tables above to do this. 

			(pbDestBits[0])[nDestOffset + x] = (byLookupImage[LOBYTE(wData) & 0x7f] << 4) |
										   byLookupImage[HIBYTE(wData) & 0x7f];
			(pbDestBits[1])[nDestOffset + x] = (byLookupImage[LOBYTE(wData) / 2] << 4) |
										   byLookupImage[HIBYTE(wData) / 2];
			if (nNumDIBs == 3) {
				(pbDestBits[2])[nDestOffset + x] = (byLookupAura[LOBYTE(wData)] << 4) |
										   byLookupAura[HIBYTE(wData)];
				// Bug in drawing code does not allow the image's pixels to be black
				// in the area where the aura is.
				(pbDestBits[0])[nDestOffset + x] &= (pbDestBits[1])[nDestOffset + x];
			}
		}
		nDestOffset += nDestScanLineSize;
		pwSrc += nWordsPerSrcScanLine;
	}

	return TRUE;
}

// ============================================================================
// CChatBackdrop implementation

#if defined(AVATAR_READ)

// Loads and returns a backdrop from an open stream. This is a static function,
// use it to create backdrops.

CChatBackdrop *
CChatBackdrop::LoadBackdrop(
CAvatarStream* pStream)
{
	if (!pStream->Open ()) {
		return NULL;
	}

	// Check the header of the file.

	AVBINT16 nMagicNum;
	if (!pStream->Read16 (&nMagicNum)) {
		pStream->Close ();
		return NULL;
	}
	
	// Back up, so that the appropriate Load function gets the entire stream.
	if (!pStream->SetPosition (-2, SEEK_CUR))
	{
		pStream->Close ();
		return NULL;
	}

	// Right now there is only one kind of backdrop - the base class. 
	// However, it could come from a bitmap (BMP) file or from one of them
	// Avatar files.

	CChatBackdrop* pBackdrop = NULL;

	TRY
	{
		switch (nMagicNum) {
			case 0x4d42: 		// 'BM'
				// Back up, because the LoadFromBmp function needs the
				// entire BMP file.
				pBackdrop = new CChatBackdrop;
				if (!pBackdrop->LoadFromBmp (pStream)) {
					::AfxThrowUserException ();
				}
				break;

			case AF_MAGICNUM_NEW:
				pBackdrop = new CChatBackdrop;
				if (!pBackdrop->Load (pStream)) {
					::AfxThrowUserException ();
				}
				break;

			default:
				TRACE("Invalid file");
				::AfxThrowUserException ();
		}
	}
	CATCH_ALL(e)
	{
		if (pBackdrop) {
			delete pBackdrop;
		}
		pStream->Close ();
		return NULL;
	}
	END_CATCH_ALL

	pStream->Close ();
	
   #if defined(AVATAR_NOT_CLIENT)
    pBackdrop->m_filetype = (WORD)nMagicNum;
   #endif // AVATAR_NOT_CLIENT
   
	return pBackdrop;
}


// Loads the backdrop from a BMP file.

BOOL
CChatBackdrop::LoadFromBmp(
CAvatarStream* pStream)
{
	ASSERT (m_pDIB == NULL);

	CAvatarDIB* pDIB = NULL;
	TRY
	{
		pDIB = new CAvatarDIB;
		if (!pDIB->Load (pStream)) {
			delete pDIB;
			return FALSE;
		}
	}
	CATCH_ALL(e)
	{
		return FALSE;
	}
	END_CATCH_ALL

	m_pDIB = pDIB;
	return TRUE;
}

// Loads the backdrop from an Avatar format file. Everything in the file
// has to be right for this to work - the file has to be of type AT_BACKDROP, 
// have the expected major version number, and have an AK_BACKDROP entry.
// Also, the backdrop can only be an AIP_LOCALPALETTE or AIP_NOPALETTE image.

BOOL
CChatBackdrop::Load(
CAvatarStream* pStream)
{
	ASSERT (m_pDIB == NULL);

	long nResourcesAdjustment = 0;

	// Get the header.
	AVATARHEADER avh;
	if (pStream->Read (&avh, sizeof(avh)) != sizeof(avh)) {
		return FALSE;
	}

	// We don't need to check the magic number, it has already been verified
	// by LoadBackdrop. We do, however, need to check the type and version.

	if (avh.nType != AT_BACKDROP) {
		TRACE("Not a backdrop file");
		return FALSE;
	}
	if (HIWORD(avh.nVersion) != 0) {
		TRACE("Unsupported version number");
		return FALSE;
	}

	// This file should only have new records (except the AK_STARTDATA section on), 
	// and we are only interested in the AK_BACKDROP record.

	while (TRUE) {
		AVBINT16 nTag;
		AVBINT16 nSize = 0;
		if (!pStream->Read16 (&nTag)) {
			return FALSE;
		}

		if (nTag == AK_STARTDATA) {
			// We kept going, and never found a backdrop. This is a problem.
			TRACE("No backdrop found in file");
			return FALSE;
		}

		if (nTag >= AK_ICON_NEW) {
			if (!pStream->Read16 (&nSize)) {
				return FALSE;
			}
		} else {
			// No old tags allowed!!
			TRACE("Old tag found, not supported in backdrop file");
			return FALSE;
		}

		// Handle information tags.
		BOOL bHandled = FALSE;
		switch (nTag) {
			case AK_ORIGINAL_URL:
				bHandled = TRUE;
				if (!ReadAvFileString (pStream, &m_pszOrigURL, MAX_URL)) {
					return FALSE;
				}
				break;
			case AK_OVERRIDE_URL:
				bHandled = TRUE;
				if (!ReadAvFileString (pStream, &m_pszNewURL, MAX_URL)) {
					return FALSE;
				}
				break;
			case AK_COPYRIGHT:
				bHandled = TRUE;
				if (!ReadAvFileString (pStream, &m_pszCopyright, MAX_COPYRIGHT)) {
					return FALSE;
				}
				break;
		   #if defined(AVATAR_NOT_CLIENT)
			case AK_USAGE_FLAGS:
				bHandled = TRUE;
				if (!pStream->Read8 (&m_byUsageFlags)) {
					return FALSE;
				}
				break;
		   #endif
			case AK_OFFSET_ADJUSTMENT:
			{
				// All offsets following this record are adjusted by the given
				// signed 2-bit integer amount. This record can be used by
				// tools which do not know the file format to add or remove things 
				// in a file.
				bHandled = TRUE;
				int nValue;
				if (!pStream->Read32 ((AVBINT32 *)&nValue)) {
					return FALSE;
				}
				if (!AddResourceAdjustment(nResourcesAdjustment, nValue))
					return FALSE;
				break;
			}
		}

		if (nTag == AK_BACKDROP) {

			// The record consists of an offset, an image format, and a palette type.
			// We are using arrays here because the AvatarPoseRecord constructor
			// needs arrays, and it is more convenient this way.
			AVBINT32 dwOffset[3];
			AVBINT8  byFormat[3];
			AVBINT8  byPaletteType[3];
			if (!pStream->Read32 (dwOffset) ||
					!pStream->Read8 (byFormat) ||
					!pStream->Read8 (byPaletteType) ||
					(byPaletteType[0] != AIP_LOCALPALETTE && byPaletteType[0] != AIP_NOPALETTE)) {
				return FALSE;
			}

			if (!AdjustAvatarOffset(dwOffset[0], nResourcesAdjustment))
				return FALSE;
			dwOffset[1] = dwOffset[2] = 0;
			CPose rec (dwOffset, byFormat, byPaletteType);
			if (!rec.Load (pStream, NULL)) {
				return FALSE;
			}

			// Transfer the DIB from the record to ourselves.
			m_pDIB = rec.m_pdibs[0];
			rec.m_pdibs[0] = NULL;

			// Drop out of the loop, we have what we need.
			break;
		}

		// Skip unhandled tags
		if (!bHandled && !pStream->SetPosition (nSize, SEEK_CUR)) {
			return FALSE;
		}
	}

	return TRUE;
}

#endif // AVATAR_READ
