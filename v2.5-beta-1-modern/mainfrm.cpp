// MainFrm.cpp : implementation of the CMainFrame class
//

#include "stdafx.h"
#include "chat.h"

#include "userinfo.h"	// required by chatprot.h
#include "chatprot.h"	// for tabbar (required by chatdoc)
#include "ircproto.h"
#include "binddoc.h"	// for tabbar
#include "chatdoc.h"	// for tabbar
#include "tabbar.h"
#include "MainFrm.h"

#include "dib.h"
#include "bbox.h"
#include "pe.h"
#include "avatar.h"
#include "bodycam.h"
#include "ui.h"
#include <afxpriv.h>
#include "memblst.h"
#include "filesend.h"
#include "modernui.h"
#include "transportuibridge.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

extern CChatApp theApp;

/////////////////////////////////////////////////////////////////////////////
// CMainFrame

IMPLEMENT_DYNAMIC(CMainFrame, CMDIFrameWnd)

BEGIN_MESSAGE_MAP(CMainFrame, CMDIFrameWnd)
	//{{AFX_MSG_MAP(CMainFrame)
	ON_WM_CREATE()
	ON_COMMAND_EX(ID_VIEW_TOOLBAR, OnBarCheck)
	ON_COMMAND(ID_VIEW_STATUS_BAR, OnViewStatusBar)
	ON_UPDATE_COMMAND_UI(ID_VIEW_STATUS_BAR, OnUpdateViewStatusBar)
	ON_UPDATE_COMMAND_UI(ID_FILE_PRINT_PREVIEW, OnUpdateFilePrintPreview)
	ON_WM_TIMER()
	ON_COMMAND_EX(ID_WINDOW_TILE_HORZ, OnMDIWindowCmd)
	ON_COMMAND_EX(ID_WINDOW_TILE_VERT, OnMDIWindowCmd)
	ON_COMMAND(ID_WINDOW_TILE_AUTO, OnWindowTileAuto)
	ON_UPDATE_COMMAND_UI(ID_WINDOW_TILE_AUTO, OnUpdateWindowTileAuto)
	ON_WM_SIZE()
	ON_WM_QUERYNEWPALETTE()
	ON_WM_PALETTECHANGED()
	ON_WM_SYSCOLORCHANGE()
	ON_WM_MENUSELECT()
	ON_WM_SETTINGCHANGE()
	//}}AFX_MSG_MAP
	ON_WM_CLOSE()
	ON_MESSAGE(WM_COMICCHAT_NETWORK_EVENT, OnComicChatNetworkEvent)
	ON_MESSAGE(WM_DPICHANGED, OnDpiChanged)
	ON_MESSAGE(WM_THEMECHANGED, OnThemeChanged)
//	ON_MESSAGE(WM_SETMESSAGESTRING, OnSetMessageString)
END_MESSAGE_MAP()

LRESULT CMainFrame::OnComicChatNetworkEvent(WPARAM, LPARAM lParam)
{
	// The transport thread posts only a wakeup. Immutable, generation-tagged
	// events are drained and converted into MFC state here on the UI thread.
	serverConn.PollNetworkEvents(lParam);
	DrainIrcv3UiEvents();
	RefreshConnectionIndicators();
	return 0;
}

void CMainFrame::DrainIrcv3UiEvents()
{
	const auto result = comic_chat::legacy_ui::DrainIrcv3EventsForUi(serverConn,
		[this](Ircv3AdapterEvent event) {
			m_lastIrcv3UiEvent = std::move(event);
			// Descendant handlers must copy any state they retain. Delivery is
			// synchronous, so the complete tagged MessageContext remains valid.
			SendMessageToAllChildWindows(WM_COMICCHAT_IRCV3_EVENT,
				static_cast<WPARAM>(m_lastIrcv3UiEvent->event.type),
				reinterpret_cast<LPARAM>(&*m_lastIrcv3UiEvent));
		});
	if (result.rejected != 0)
		TRACE("IRCv3 UI bridge rejected %zu event(s).\n", result.rejected);
	if (result.dropped_before_delivery != m_reportedDroppedIrcv3Events) {
		TRACE("IRCv3 adapter dropped %llu event(s) before UI delivery.\n",
			static_cast<unsigned long long>(result.dropped_before_delivery));
		m_reportedDroppedIrcv3Events = result.dropped_before_delivery;
	}
}

static UINT indicators[] =
{
	ID_SEPARATOR,           // room/action status
	ID_SEPARATOR,			// member count
	ID_SEPARATOR,			// transport/reconnect state
	ID_SEPARATOR,			// TLS verification state
	ID_SEPARATOR,			// SASL/account state
};

/////////////////////////////////////////////////////////////////////////////
// CMainFrame construction/destruction

CMainFrame::CMainFrame()
{
	// TODO: add member initialization code here
//		_CrtSetBreakAlloc(399);


	m_bOleShuttingDown = FALSE;
	m_uCurrentDpi = 96;


}

CMainFrame::~CMainFrame()
{
}

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct)
{
	if (CMDIFrameWnd::OnCreate(lpCreateStruct) == -1)
		return -1;
	
	cui.m_pvFrameWnd = this;					// save this away for later use

	if (!m_wndToolBar.Create(this, theApp.m_bDoCB32))
	{
		TRACE0("Failed to create toolbar\n");
		return -1;      // fail to create
	}

	cui.m_pvToolBarWnd = &m_wndToolBar;
	if (!(theApp.m_iShowBars & SB_TOOLBAR_ANY)) m_wndToolBar.ShowWindow(SW_HIDE);

//  Create our own status bar so that we can configure the panes
	if (!m_wndStatusBar.Create(this) ||
//	if (!m_wndStatusBar.Create(this,WS_CHILD | WS_VISIBLE | CBRS_BOTTOM,ID_CCHAT_STATUSBAR) ||
		!m_wndStatusBar.SetIndicators(indicators,
		  sizeof(indicators)/sizeof(UINT)))
	{
		TRACE0("Failed to create status bar\n");
		return -1;      // fail to create
	}

	cui.m_pvStatusBarWnd = &m_wndStatusBar;
	if (!(theApp.m_iShowBars & SB_STATUSBAR)) m_wndStatusBar.ShowWindow(SW_HIDE);

	m_wndStatusBar.SetFont(&theApp.m_fontGui);  // give status bar and mainframe correct font
//	SetFont(&theApp.m_fontGui);					// doesn't seem to work for title bar.  Why?

	UINT	nID;
	UINT	nStyle;
	int		cxWidth;
	CString	strWidth;

	strWidth.LoadString(IDS_MEMBER_COUNT_WIDTH);
	m_wndStatusBar.GetPaneInfo(1, nID, nStyle, cxWidth);
	m_wndStatusBar.SetPaneInfo(1, nID, nStyle, atoi(strWidth));

	EnableDocking(CBRS_ALIGN_ANY);

	if ((m_wndToolBar.GetStyle () & WS_VISIBLE) != 0)
	{
		CControlBar * pBar = GetControlBar (AFX_IDW_DOCKBAR_TOP);
		if (pBar != NULL)
		{
			pBar->SetBarStyle (pBar->GetBarStyle () & ~CBRS_BORDER_ANY);
		}
	}

	CString tabTitle;
	tabTitle.LoadString(IDS_TABTITLE);
	if (!m_wndTabBar.Create(NULL, tabTitle, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
				CRect(0, 0, 100, 100), this, 81)) {
		TRACE0("Failed to create tabbar\n");
		return -1;      // fail to create
	}

	m_wndTabBar.SetBarStyle((m_wndTabBar.GetBarStyle() | CBRS_SIZE_DYNAMIC) & ~(CBRS_BORDER_TOP | CBRS_BORDER_BOTTOM | CBRS_BORDER_LEFT | CBRS_BORDER_RIGHT));
	m_wndTabBar.EnableDocking(CBRS_ALIGN_TOP | CBRS_ALIGN_BOTTOM);
	DockControlBar(&m_wndTabBar, AFX_IDW_DOCKBAR_TOP);
	cui.m_pvTabBar = &m_wndTabBar;
	if (theApp.m_bDoCB32 || !(theApp.m_flags1 & F1_SHOWTABBAR)) m_wndTabBar.ShowWindow(SW_HIDE);

	m_uCurrentDpi = comic_chat::modern_ui::DpiForWindow(m_hWnd);
	ApplyModernMetrics(m_uCurrentDpi);
	RefreshModernAppearance();
	RefreshConnectionIndicators();

	return 0;
}

BOOL CMainFrame::PreCreateWindow(CREATESTRUCT& cs)
{
	// TODO: Modify the Window class or styles here by modifying
	//  the CREATESTRUCT cs
	if (theApp.m_cxFrame && theApp.m_cyFrame) {
		// ShankuN 6/2/98
		// Make sure window is visible.

		CRect rect (theApp.m_xFrame, theApp.m_yFrame, 
				theApp.m_xFrame + theApp.m_cxFrame, theApp.m_yFrame + theApp.m_cyFrame);
		MakeRectVisibleOnScreen (&rect);

		// Use the adjusted rect for BOTH position and size: MakeRectVisibleOnScreen
		// may have shrunk an oversized (e.g. stale, different-DPI) placement to fit
		// the work area.
		cs.x = rect.left;
		cs.y = rect.top;
		cs.cx = rect.Width();
		cs.cy = rect.Height();
		// note: maximization setting happens in CChatApp.InitInstance
	}

	return CMDIFrameWnd::PreCreateWindow(cs);
}

// Save user settings (fonts, colors, panel layout, ...) at frame close, BEFORE
// the window teardown. On a modern DEBUG build that teardown can trip framework
// assertions that prevent CChatApp::ExitInstance (the original save point) from
// running; saving here guarantees the settings persist either way. Harmless and
// idempotent in Release, where ExitInstance also runs normally.
void CMainFrame::OnClose()
{
	theApp.SaveToReg(FALSE /*bShort*/);
	CMDIFrameWnd::OnClose();
}





LRESULT CMainFrame::WindowProc( UINT message, WPARAM wParam, LPARAM lParam )
// This is a debug hook for catching a really weird condtion that was occuring before
// I fixed the UIActivate... handshaking with XCChat. I left it in here just for 
// informations sake if it happens again. This function and the simular ones in 
// mainfrm.cpp can probably be removed at some point down the road.
//
// This is only active after CDocObjectServerDoc::XDocOleObject::Close has been called 
// for DEBUG builds. The rest of the time it just passes everything
{

	// are we shuting down an embedded server?
	if( g_bOleShuttingDown )
	{
		HWND hwndActive;
		HWND hwndMDI = m_hWndMDIClient;
		if( hwndMDI != NULL )
		{
			hwndActive = (HWND)::SendMessage( hwndMDI, WM_MDIGETACTIVE, 0, NULL );
			if (hwndActive != NULL && !IsWindow( hwndActive ))
			{
				TRACE1( "BAD MDI CLIENT ACTIVE WINDOW HANDLE %x\n", hwndActive );
				::SendMessage (hwndMDI, WM_MDIACTIVATE, 0, 0);
			}
		}
	}

	// pass message on to MFC
	return( CMDIFrameWnd::WindowProc( message, wParam, lParam ) );
}




BOOL  CMainFrame::PreTranslateMessage( MSG* pMsg )
// This is a debug hook for catching a really weird condtion that was occuring before
// I fixed the UIActivate... handshaking with XCChat. I left it in here just for 
// informations sake if it happens again. This function and the simular ones in 
// mainfrm.cpp can probably be removed at some point down the road.
//
// This is only active after CDocObjectServerDoc::XDocOleObject::Close has been called 
// for DEBUG builds. The rest of the time it just passes everything
{

	// are we shuting down an embedded server?
	if( g_bOleShuttingDown )
	{

		HWND hwndActive;
		HWND hwndMDI = m_hWndMDIClient;
		if( hwndMDI != NULL )
		{
			hwndActive = (HWND)::SendMessage( hwndMDI, WM_MDIGETACTIVE, 0, NULL );
			if (hwndActive != NULL && !IsWindow( hwndActive ))
			{
				TRACE1( "BAD MDI CLIENT ACTIVE WINDOW HANDLE %x\n", hwndActive );
				::SendMessage (hwndMDI, WM_MDIACTIVATE, 0, 0);
			}
		}
	}

	// pass message on to MFC
	return( CMDIFrameWnd::PreTranslateMessage( pMsg ) );
}



/////////////////////////////////////////////////////////////////////////////
// CMainFrame diagnostics

#ifdef _DEBUG
void CMainFrame::AssertValid() const
{
	CMDIFrameWnd::AssertValid();
}

void CMainFrame::Dump(CDumpContext& dc) const
{
	CMDIFrameWnd::Dump(dc);
}

#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// CMainFrame message handlers


void CMainFrame::OnViewStatusBar() 
{
	m_wndStatusBar.ShowWindow((m_wndStatusBar.GetStyle() & WS_VISIBLE)==0);
	RecalcLayout();
}

void CMainFrame::OnUpdateViewStatusBar(CCmdUI* pCmdUI) 
{
	pCmdUI->SetCheck((m_wndStatusBar.GetStyle() & WS_VISIBLE)!=0);
}


void CMainFrame::OnUpdateFilePrintPreview(CCmdUI* pCmdUI) 
{
	pCmdUI->Enable(FALSE);
}


void CMainFrame::OnMenuSelect(UINT nItemID, UINT nFlags, HMENU hSysMenu)
{
	if (ID_MACRO_A1 <= nItemID && ID_MACRO_A9 >= nItemID)
		SetMessageText(ID_MACRO_A0);
	else
		CMDIFrameWnd::OnMenuSelect(nItemID, nFlags, hSysMenu);
}


void CMainFrame::GetMessageString( UINT nID, CString& rMessage ) const {
	if (nID == AFX_IDS_IDLEMESSAGE)
		rMessage = theApp.m_strStatusPane[0];
	else CMDIFrameWnd::GetMessageString(nID, rMessage);
}


void CMainFrame::OnTimer(UINT nIDEvent) 
{
	void OnEaster();

	switch (nIDEvent)
	{
	case g_uNotifsDaemonTimer:
		theApp.m_dynaNotifs.OnNotifsDaemonTimer();
		break;
	case g_uRulesDaemonTimer:
		theApp.m_dynaRules.OnRulesDaemonTimer();
		break;
	case g_uDelayedRulesTimer:
		theApp.m_delayedRules.bExecuteActions();
		break;
	case ID_ISIRCXTIMEOUT:
		theApp.IsIrcXTimeout();
		break;
	case EASTER_TIMER:
		OnEaster();
		break;
	}

	CMDIFrameWnd::OnTimer(nIDEvent);
	DrainIrcv3UiEvents();
	RefreshConnectionIndicators();
}

void CMainFrame::ApplyModernMetrics(UINT dpi)
{
	m_uCurrentDpi = dpi ? dpi : 96;
	g_screenDpi = static_cast<int>(m_uCurrentDpi); // compatibility for legacy pixel surfaces
	const auto metrics = comic_chat::modern_ui::MetricsForDpi(m_uCurrentDpi);
	if (HFONT font = comic_chat::modern_ui::UiFont(m_uCurrentDpi))
		m_wndStatusBar.SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

	UINT id = 0;
	UINT style = 0;
	int width = 0;
	m_wndStatusBar.GetPaneInfo(0, id, style, width);
	m_wndStatusBar.SetPaneInfo(0, id, style | SBPS_STRETCH, comic_chat::modern_ui::Scale(160, m_uCurrentDpi));
	CString memberWidth;
	memberWidth.LoadString(IDS_MEMBER_COUNT_WIDTH);
	m_wndStatusBar.GetPaneInfo(1, id, style, width);
	m_wndStatusBar.SetPaneInfo(1, id, style & ~SBPS_STRETCH,
		comic_chat::modern_ui::Scale(atoi(memberWidth), m_uCurrentDpi));
	const int semanticWidths[] = {
		comic_chat::modern_ui::Scale(152, m_uCurrentDpi),
		comic_chat::modern_ui::Scale(112, m_uCurrentDpi),
		comic_chat::modern_ui::Scale(112, m_uCurrentDpi),
	};
	for (int pane = 2; pane < 5; ++pane) {
		m_wndStatusBar.GetPaneInfo(pane, id, style, width);
		m_wndStatusBar.SetPaneInfo(pane, id, style & ~SBPS_STRETCH, semanticWidths[pane - 2]);
	}
	m_wndStatusBar.GetStatusBarCtrl().SetMinHeight(metrics.status_height);
	theApp.InitStatusIcons(m_uCurrentDpi, m_hWnd);
	if (m_wndToolBar.GetSafeHwnd())
		m_wndToolBar.RefreshModernImages(m_uCurrentDpi);
	m_wndTabBar.ApplyModernMetrics(m_uCurrentDpi);
	RecalcLayout();
}

void CMainFrame::RefreshModernAppearance()
{
	comic_chat::modern_ui::ApplyWindowTheme(m_hWnd, false);
	comic_chat::modern_ui::ApplyWindowTheme(m_wndStatusBar.m_hWnd, false);
	m_wndTabBar.RefreshSystemAppearance();
	if (m_wndToolBar.GetSafeHwnd())
	{
		comic_chat::modern_ui::ApplyWindowTheme(m_wndToolBar.m_hWnd, true);
		m_wndToolBar.RefreshModernImages(m_uCurrentDpi);
	}
	SendMessageToAllChildWindows(WM_THEMECHANGED, 0, 0);
	RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void CMainFrame::RefreshConnectionIndicators()
{
	comic_chat::modern_ui::ConnectionState state;
	state.transport = comic_chat::modern_ui::TransportStateFor(serverConn.GetTransportState());
	state.secure = serverConn.IsSecureTransport() != FALSE;
	state.registration_finished = serverConn.RegistrationFinished();
	state.sasl_succeeded = serverConn.SaslSucceeded();
	const auto labels = comic_chat::modern_ui::LabelsFor(state);
	std::string transportLabel = labels.transport;
	const auto transfers = GetFileTransferSnapshots();
	std::size_t activeTransfers = 0;
	bool transferNeedsAttention = false;
	for (const auto& transfer : transfers) {
		switch (transfer.state) {
		case FileTransferPhase::awaiting_peer:
		case FileTransferPhase::connecting:
		case FileTransferPhase::transferring:
			++activeTransfers;
			break;
		case FileTransferPhase::failed:
		case FileTransferPhase::timed_out:
			transferNeedsAttention = true;
			break;
		default:
			break;
		}
	}
	if (transferNeedsAttention)
		transportLabel += " | File transfer failed";
	else if (activeTransfers == 1)
		transportLabel += " | 1 file transfer";
	else if (activeTransfers > 1)
		transportLabel += " | " + std::to_string(activeTransfers) + " file transfers";
	const char* values[] = {
		transportLabel.c_str(), labels.security.c_str(), labels.authentication.c_str()};
	for (int index = 0; index < 3; ++index) {
		if (m_modernStatusText[index] == values[index]) continue;
		m_modernStatusText[index] = values[index];
		m_wndStatusBar.SetPaneText(index + 2, m_modernStatusText[index]);
	}
}

LRESULT CMainFrame::OnDpiChanged(WPARAM wParam, LPARAM lParam)
{
	const UINT dpi = HIWORD(wParam) ? HIWORD(wParam) : LOWORD(wParam);
	if (const RECT* suggested = reinterpret_cast<const RECT*>(lParam)) {
		SetWindowPos(NULL, suggested->left, suggested->top,
			suggested->right - suggested->left, suggested->bottom - suggested->top,
			SWP_NOACTIVATE | SWP_NOZORDER);
	}
	ApplyModernMetrics(dpi);
	// Child MFC views do not receive top-level WM_DPICHANGED automatically.
	// Recompute per-instance pixel surfaces (notably bodycams) from 96-DPI
	// constants without forwarding the top-level suggested rectangle.
	SendMessageToAllChildWindows(WM_DPICHANGED, MAKEWPARAM(dpi, dpi), 0);
	RefreshModernAppearance();
	return 0;
}

LRESULT CMainFrame::OnThemeChanged(WPARAM, LPARAM)
{
	RefreshModernAppearance();
	RefreshConnectionIndicators();
	return 0;
}

void CMainFrame::OnSettingChange(UINT uFlags, LPCTSTR lpszSection)
{
	CMDIFrameWnd::OnSettingChange(uFlags, lpszSection);
	RefreshModernAppearance();
}

BOOL CMainFrame::OnBarCheck(UINT nID)
{
	if (nID == ID_VIEW_TOOLBAR)
	{
		GetToolBar ()->ToggleBar (CHAT_TOOLBAR_WHOLE);
		return TRUE;
	}
	else
	{
		return CFrameWnd::OnBarCheck (nID);
	}
}

BOOL
CMainFrame::OnMDIWindowCmd(
UINT nID)
{
	// Let CMDIFrameWnd's implementation tile the window.
	BOOL b = CMDIFrameWnd::OnMDIWindowCmd (nID);
	// We just need to know what the user's last tiling option was.
	switch (nID)
	{
		case ID_WINDOW_TILE_HORZ:
			theApp.m_flags0 &= ~F0_AUTOARRANGEISVERT;
			break;
		case ID_WINDOW_TILE_VERT:
			theApp.m_flags0 |= F0_AUTOARRANGEISVERT;
			break;
	}
	return b;
}

void
CMainFrame::OnWindowTileAuto()
{
	theApp.m_flags0 ^= F0_AUTOARRANGEWNDS;
	AutoArrangeWindows ();	// Auto arrange if we turned the option on.
}

void
CMainFrame::OnUpdateWindowTileAuto(
CCmdUI* pCmdUI)
{
	pCmdUI->SetCheck ((theApp.m_flags0 & F0_AUTOARRANGEWNDS) != 0);
}

void 
CMainFrame::AutoArrangeWindows()
{
	if ((theApp.m_flags0 & F0_AUTOARRANGEWNDS) != 0)
	{
		PostMessage (WM_COMMAND, 
			((theApp.m_flags0 & F0_AUTOARRANGEISVERT) != 0) ? ID_WINDOW_TILE_VERT : ID_WINDOW_TILE_HORZ);
	}
}

void 
CMainFrame::OnSize(
UINT type, 
int cx, 
int cy)
{
	CMDIFrameWnd::OnSize (type, cx, cy);
	AutoArrangeWindows ();
}

struct BROADCASTMESSAGE
{
	UINT message;
	WPARAM wParam;
	LPARAM lParam;
};

BOOL 
CMainFrame::SendBroadcastProc(
HWND hwnd,
LPARAM lParam)
{
	BROADCASTMESSAGE * pMsg = (BROADCASTMESSAGE*)lParam;
	CWnd::FromHandle (hwnd)->SendMessageToDescendants (pMsg->message, pMsg->wParam, pMsg->lParam);
	return TRUE;
}

void 
CMainFrame::SendMessageToAllChildWindows(
UINT nMsg,
WPARAM wParam,
LPARAM lParam)
{
	BROADCASTMESSAGE msg;
	msg.message = nMsg;
	msg.wParam = wParam;
	msg.lParam = lParam;

	EnumThreadWindows (GetCurrentThreadId (), SendBroadcastProc, (LPARAM)&msg);
}

void 
CMainFrame::OnPaletteChanged(
CWnd* pFocusWnd)
{
	// Are we even in palette mode?
	{
		CClientDC dc(this);
		if ((dc.GetDeviceCaps (RASTERCAPS) & RC_PALETTE) == 0)
			return;
	}

	SendMessageToAllChildWindows (WM_PALETTECHANGED, (WPARAM)pFocusWnd->GetSafeHwnd ());
	CMDIFrameWnd::OnPaletteChanged (pFocusWnd);
}

BOOL
CMainFrame::OnQueryNewPalette()
{
	return theApp.QueryNewPaletteCommon (this);
}

void
CMainFrame::OnSysColorChange()
{
	SendMessageToAllChildWindows (WM_SYSCOLORCHANGE);
	CMDIFrameWnd::OnSysColorChange();
	RefreshModernAppearance();
}
