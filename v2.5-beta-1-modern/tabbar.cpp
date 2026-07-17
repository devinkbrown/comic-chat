#include "stdafx.h"

#include "chat.h"
#include "userinfo.h"
#include "chatprot.h"
#include "binddoc.h"
#include "chatdoc.h"

#include "tabbar.h"
#include "resource.h"
#include "ui.h"
#include "protsupp.h"
#include "modernicons.h"
#include "modernui.h"

extern CChatApp theApp;


/////////////////////////////////////////////////////////////////////////////
// CTabBar

CTabBar::CTabBar()
{
	m_bDeleteFont	= FALSE;
	m_uDpi = 96;
	m_nLayoutLength = 0;
	m_lLargestTab	= comic_chat::modern_ui::Scale(64, m_uDpi);

	LOGFONT	logFont;
	theApp.m_fontGui.GetLogFont(&logFont);

	CString strRoomTabFontWeight;
	strRoomTabFontWeight.LoadString(IDS_ROOMTAB_FONTWEIGHT);
	logFont.lfWeight = atoi(strRoomTabFontWeight);
	if (m_font.CreateFontIndirect(&logFont))
		m_bDeleteFont = TRUE;
}

CTabBar::~CTabBar()
{
	if (m_bDeleteFont)
		m_font.DeleteObject();
}


BEGIN_MESSAGE_MAP(CTabBar, CControlBar)
	//{{AFX_MSG_MAP(CTabBar)
	ON_WM_CREATE()
	ON_WM_SIZE()
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB1, OnSelchangeTab)
	ON_WM_ERASEBKGND()
	//}}AFX_MSG_MAP
	ON_NOTIFY(TCN_KEYDOWN,IDC_TAB1,OnKeyDown)
END_MESSAGE_MAP()


/////////////////////////////////////////////////////////////////////////////
// CTabBar message handlers

BOOL CTabBar::OnEraseBkgnd(CDC* pDC) 
{
	// For some unknown reason, pDC sometimes is a CClientDC that's invalid (doesn't
	// have a m_hWnd associated with it.  So we need to create our own.  Sigh. Investigate!
	// Seems to happen when we hide the tab control
//	CClientDC dc(this);
	const auto palette = comic_chat::modern_ui::PaletteForWindow(m_hWnd);
	DWORD clr = comic_chat::modern_ui::ToColorRef(palette.surface);
	RECT rect;

	GetClientRect(&rect);
	pDC->FillSolidRect(&rect, clr);
	return TRUE;
}


// REGISB 10/24/97 This method is never called - removing
//CSize CTabBar::CalcFixedLayout(BOOL bStretch, BOOL bHorz) {
//	OutputDebugString("CTabBar::CalcFixedLayout\n");
//	CWnd *parent = GetParent();
//	RECT rect;
//	parent->GetClientRect(&rect);
//
//	return bHorz ? CSize(rect.right, 25) : CSize(20, rect.bottom);
//}


CSize CTabBar::CalcDynamicLayout(int nLength, DWORD dwMode)
{
//#ifdef DEBUG
//	BOOL b = dwMode & LM_STRETCH;
//	BOOL c = dwMode & LM_HORZ;
//	BOOL d = dwMode & LM_MRUWIDTH;
//	BOOL e = dwMode & LM_HORZDOCK;
//	BOOL f = dwMode & LM_VERTDOCK;
//	BOOL g = dwMode & LM_LENGTHY;
//	BOOL h = dwMode & LM_COMMIT;
//	TRACE("CTabBar::CalcDynamicLayout dwMode=%d, nLength=%d, iLength=%d\n", dwMode, nLength, iLength);
//	TRACE("LM_STRETCH=%d, LM_HORZ=%d, LM_MRUWIDTH=%d, LM_HORZDOCK=%d, LM_VERTDOCK=%d, LM_LENGTHY=%d, LM_COMMIT= %d\n", b,c,d,e,f,g,h);
//#endif // DEBUG

	CWnd*	pParentWnd = GetDockingFrame();
	RECT	rect;

	ASSERT(pParentWnd);
	pParentWnd->GetWindowRect(&rect);
	const auto metrics = comic_chat::modern_ui::MetricsForDpi(m_uDpi);
	const int trailingSpace = comic_chat::modern_ui::Scale(64, m_uDpi);

	if (dwMode == LM_HORZ)
		m_nLayoutLength = max(nLength, m_lLargestTab + trailingSpace);
	else
		if (dwMode == LM_HORZ+LM_HORZDOCK && nLength == 0)
			m_nLayoutLength = max(m_nLayoutLength, m_lLargestTab + trailingSpace);
		else
			if (dwMode == LM_HORZ+LM_HORZDOCK && nLength == -1)
			{
				m_nLayoutLength = m_nLayoutLength ? m_nLayoutLength :
					max(rect.right - rect.left - comic_chat::modern_ui::Scale(100, m_uDpi),
						m_lLargestTab + trailingSpace);
				return CSize(rect.right - rect.left, metrics.tab_height + metrics.border);
			}

	return CSize(m_nLayoutLength, metrics.tab_height + metrics.border);
}


int CTabBar::OnCreate(LPCREATESTRUCT lpCreateStruct) 
{
	if (CControlBar::OnCreate(lpCreateStruct) == -1)
		return -1;

	RECT r;
	GetClientRect(&r);
	m_uDpi = comic_chat::modern_ui::DpiForWindow(m_hWnd);
	r.top = comic_chat::modern_ui::Scale(3, m_uDpi);
//	r.bottom = 15;
	
	m_tabCtrl.Create(
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_HOTTRACK | TCS_TOOLTIPS,
		r, this, IDC_TAB1);
	m_tabCtrl.SetWindowText("Conversation tabs");

	if (m_bDeleteFont)
		m_tabCtrl.SetFont(&m_font, FALSE);

	ApplyModernMetrics(m_uDpi);
	RefreshSystemAppearance();
	
	return 0;
}


void CTabBar::OnSize(UINT nType, int cx, int cy) 
{
	// OutputDebugString("CTabBar::OnSize\n");
	CControlBar::OnSize(nType, cx, cy);
	RECT rect;
	m_tabCtrl.GetWindowRect(&rect);
	m_tabCtrl.SetWindowPos(NULL, 0, 0, cx, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER);
}


void CTabBar::OnSelchangeTab(NMHDR* pNMHDR, LRESULT* pResult) 
{
	//OutputDebugString("CTabBar::OnSelchangeTab\n");

	BOOL bHasFocus = ::GetFocus () == pNMHDR->hwndFrom;

	int sel = m_tabCtrl.GetCurSel();
	ASSERT(sel >= 0);
	ActivateWindow(GetTabDoc(sel));
	if (bHasFocus)
		::SetFocus (pNMHDR->hwndFrom);	// Restore focus back to tab control.

	*pResult = 0;
}


CChatDoc *CTabBar::GetTabDoc(int index) {
	TC_ITEM tcItem;
	tcItem.mask = TCIF_PARAM;
	m_tabCtrl.GetItem(index, &tcItem);
	return ((CChatDoc *)tcItem.lParam);
}


void CTabBar::AddMDITab(const char *szChanName, CChatDoc *pDoc, BOOL bSelectIt /* = TRUE */)
{
	//OutputDebugString("CTabBar::AddMDITab\n");
	RECT	rect;
	TC_ITEM tcItem;
	CString strTabName;
	int		iPlace = 0, nTabs = m_tabCtrl.GetItemCount();

	if (nTabs == 0)
		m_tabCtrl.ShowWindow(SW_SHOWNOACTIVATE); // keep invisible until tabs added due to refresh bug

	if (!pDoc->m_bStatusView)
		for (int i = 0; i < nTabs; i++)
		{
			CChatDoc *doc2 = GetTabDoc(i);
			GetTabString(i, strTabName);
			if (stricmp(szChanName, strTabName) < 0 && !doc2->m_bStatusView)
				break;
			else
				iPlace++;
		}

	tcItem.mask    = TCIF_TEXT | TCIF_IMAGE | TCIF_PARAM;
	tcItem.pszText = (char*) szChanName;
	tcItem.iImage  = pDoc->m_bStatusView ? 2 : 0; // XXX
	tcItem.lParam  = reinterpret_cast<LPARAM>(pDoc);

	m_tabCtrl.InsertItem(iPlace, &tcItem);
	if (bSelectIt)
		m_tabCtrl.SetCurSel(iPlace);
	m_tabCtrl.GetItemRect(iPlace, &rect);

	if ((rect.right - rect.left) > m_lLargestTab)
		m_lLargestTab = rect.right - rect.left;

	pDoc->m_bNewContent = FALSE;
}


void SetActiveTab(CChatDoc *pDoc)
{
	//OutputDebugString("SetActiveTab\n");
	CTabBar *pTB = GetTabBar();
	if (!pTB)
		return;
	int i = pTB->FindTabNum(pDoc);
	if (i > -1)
		pTB->m_tabCtrl.SetCurSel(i);
}


void CTabBar::SetTabIcon(int tabNum, int iIcon) {
	//OutputDebugString("CTabBar::SetTabIcon\n");
	TC_ITEM tcItem;
	tcItem.mask = TCIF_IMAGE;
	tcItem.iImage = iIcon;
	m_tabCtrl.SetItem(tabNum, &tcItem);
}


void CTabBar::GetTabString(int i, CString &name) {
	//OutputDebugString("CTabBar::GetTabString\n");
	TC_ITEM tcItem;
	char textBuff[100];
	tcItem.mask = TCIF_TEXT;
	tcItem.cchTextMax = sizeof(textBuff);
	tcItem.pszText = textBuff;
	m_tabCtrl.GetItem(i, &tcItem);
	name = tcItem.pszText;
}


void CTabBar::DelMDITab(int iTab) {
	//OutputDebugString("CTabBar::DelMDITab\n");
	if (iTab > -1)
	{
		RECT rect;
	
		m_tabCtrl.GetItemRect(iTab, &rect);
		m_tabCtrl.DeleteItem(iTab);

		int nTabs = m_tabCtrl.GetItemCount();
		if (0 == nTabs)
		{
			m_tabCtrl.ShowWindow(SW_HIDE);	// keep invisible w/o tabs, due to refresh bug
			m_lLargestTab = comic_chat::modern_ui::Scale(64, m_uDpi);
		}
		else
			if ((rect.right - rect.left) == m_lLargestTab)
			{
				// largest tab was just removed
				m_lLargestTab = comic_chat::modern_ui::Scale(64, m_uDpi);
				for (int i = 0; i < nTabs; i++)
				{
					m_tabCtrl.GetItemRect(i, &rect);
					if ((rect.right - rect.left) > m_lLargestTab)
						m_lLargestTab = rect.right - rect.left;
				}
			}
	}
}

void CTabBar::ApplyModernMetrics(UINT dpi)
{
	m_uDpi = dpi ? dpi : 96;
	const auto metrics = comic_chat::modern_ui::MetricsForDpi(m_uDpi);
	if (HFONT font = comic_chat::modern_ui::UiFont(m_uDpi))
		m_tabCtrl.SendMessage(WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	// Preserve native text-derived widths; padding supplies a DPI-scaled target
	// without the zero-width TCM_SETITEMSIZE legacy shortcut.
	m_tabCtrl.SetPadding(CSize(metrics.tab_padding,
		max(1, (metrics.target - metrics.icon) / 2)));
	if (comic_chat::modern_ui::BuildStripImageList(
			m_images, IDB_TABS, metrics.icon, 4, m_tabCtrl.m_hWnd))
		m_tabCtrl.SetImageList(&m_images);
	m_lLargestTab = max(m_lLargestTab, static_cast<LONG>(comic_chat::modern_ui::Scale(64, m_uDpi)));
	m_nLayoutLength = 0;
	Invalidate();
}

void CTabBar::RefreshSystemAppearance()
{
	comic_chat::modern_ui::ApplyWindowTheme(m_hWnd, true);
	const auto metrics = comic_chat::modern_ui::MetricsForDpi(m_uDpi);
	if (comic_chat::modern_ui::BuildStripImageList(
			m_images, IDB_TABS, metrics.icon, 4, m_tabCtrl.m_hWnd))
		m_tabCtrl.SetImageList(&m_images);
	Invalidate();
}


int CTabBar::FindTabNum(CChatDoc *doc) {
	//OutputDebugString("CTabBar::FindTabNum\n");
	int nTabs = m_tabCtrl.GetItemCount();
	for (int i = 0; i < nTabs; i++)
		if (doc == GetTabDoc(i)) return i;

	return -1;
}

void 
CTabBar::OnKeyDown(
NMHDR* pNMHDR, 
LRESULT* pResult)
{
	NMTCKEYDOWN * pnm = (NMTCKEYDOWN*)pNMHDR;
	if (pnm->wVKey == VK_TAB)
	{
		GetChatDoc ()->CycleFocus (CHATFOCUS_TABBAR, GetKeyState (VK_SHIFT) & 0x8000);
	}
}

BEGIN_MESSAGE_MAP(CTabBarTabCtrl, CTabCtrl)
	ON_WM_GETDLGCODE()
	ON_WM_CHAR()
END_MESSAGE_MAP()

UINT 
CTabBarTabCtrl::OnGetDlgCode()
{
	UINT nRet = CTabCtrl::OnGetDlgCode ();
	return nRet | DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS;
}

void 
CTabBarTabCtrl::OnChar(
UINT nChar, 
UINT nRepCnt, 
UINT nFlags)
{
	if (nChar != VK_TAB)
		ForwardToSayWnd(nChar);
}
