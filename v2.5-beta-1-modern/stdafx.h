// stdafx.h : include file for standard system include files,
//  or project specific include files that are used frequently, but
//      are changed infrequently
//

// DJK - these were necessary in 1998 when MFC 4.0's prebuilt library used the
//   old common-control struct tags (_LV_ITEMA etc.) while the SDK's commctrl.h
//   had switched to the new tags. The modern MFC library and SDK both use the
//   new tags, so remapping them here would instead BREAK linkage. Left disabled.
//#define tagLVITEMA		_LV_ITEMA
//#define tagLVFINDINFOA	_LV_FINDINFOA
//#define tagTCITEMA		_TC_ITEMA

#define VC_EXTRALEAN		// Exclude rarely-used stuff from Windows headers

#include <winsock2.h>        // Adapter error constants; libuv still owns sockets
#include <afxwin.h>         // MFC core and standard components
#include <afxext.h>         // MFC extensions
#include <afxole.h>         // MFC OLE classes
#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>			// MFC support for Windows 95 Common Controls
#endif // _AFX_NO_AFXCMN_SUPPORT

// Unicode<==>ANSI conversion macros are in afxconv.h for MFC 4.2 on, but in
// afxpriv.h before that.
#if (_MFC_VER > 0x0410)
#include <afxconv.h>
#else
#include <afxpriv.h>
#endif
#include <afxtempl.h>

#include <process.h>
#include <wininet.h>

#include <mbstring.h>
#include <shlobj.h>
#include <mmsystem.h>

#include "dpiscale.h"
#include "safectype.h"		// make is*/to* safe for raw (signed) char args

#ifndef NOGLOBPAL
extern CPalette        ghPalette;
extern LOGPALETTE      *gpLogPal;
#endif NOGLOBPAL

#include "chicdial.h"
#include "coolbar.h"

// The original source used a private two-argument ASSERT(expression, message)
// extension. Current MFC exposes the standard single-argument ASSERT macro,
// and the conforming preprocessor correctly rejects the extra argument. Keep
// the diagnostic text without relying on the legacy preprocessor extension.
#ifdef _DEBUG
#define CC_ASSERT(expression, message) \
	do { \
		if (!(expression)) { \
			TRACE("Comic Chat assertion: %s\n", (message)); \
			ASSERT(expression); \
		} \
	} while (false)
#else
#define CC_ASSERT(expression, message) ((void)0)
#endif
