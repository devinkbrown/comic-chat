#ifndef __UI_H__
#define __UI_H__

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#define GetCharSelBodyCam()	((CBodyCam *)cui.GetCharSelBodyCamPv())
#define GetStatusBar()	((CStatusBar *)cui.GetStatusBarPv())
#define GetToolBar()	((CChatToolBar *)cui.GetToolBarPv())
#define GetClientDC()	((CClientDC *)cui.GetClientDCPv())
#define GetChatView()	((CChatView *)cui.GetChatViewPv())
#define GetFrame()		((CFrameWnd *)cui.GetFramePv())
#define GetRoomList()	((CRoomList *)cui.GetRoomListPv())
#define GetUserList()	((CUserList *)cui.GetUserListPv())
#define GetChatDoc()    ((CChatDoc  *)cui.GetChatDocPv())
#define GetPersonalPage() ((CPersonalPage *)cui.GetPersonalPagePv())
#define GetWhisperBox()	((CWhisperBox *)cui.GetWhisperBoxPv())
#define GetNotifBox()	((CNotificationUsers *)cui.GetNotifBoxPv())
#define GetFocusedDoc() ((CChatDoc *) cui.GetFocusedDocPv())
#define GetTabBar()		((CTabBar *) cui.GetTabBarPv())
#define GetOutBuff()	(cui.GetOutBuffSz())
#define GetOutBuffLen()	(cui.GetOutBuffLenN())
#define GetIrcProto()	((CIrcProto *)cui.GetIrcProtoPv())
#define GetDefaultProto() ((CRoomInfo *)cui.GetIrcProtoPv())
#define GetStatusView() ((CWnd *)cui.GetStatusViewPv())

class CUI
{
public:

////////////////////////////////////////////////////////////////
//
	// REVIEW: think about this
    void *	m_pvCharSelBodyCamWnd;
    void *  m_pvStatusBarWnd;
	void *	m_pvToolBarWnd;
	void *	m_pvClientDC;
	void *	m_pvChatView;
	void *	m_pvFrameWnd;
	void *	m_pvRoomList;
	void *  m_pvUserList;
	void *  m_pvChatDoc;
	void *  m_pvPersonalPage;
	void *	m_pvWhisperBox;
	void *	m_pvNotifBox;
	void *	m_pvFocusedDoc;
	void *	m_pvTabBar;
	void *	m_pvIrcProto;
	void *	m_pvStatusView;
	char *  m_szOutBuff;
	short   m_nOutBuffLen;

////////////////////////////////////////////////////////////////
//

	CUI()
	{
		m_pvWhisperBox		= NULL;
		m_pvNotifBox		= NULL;
		m_pvChatView		= NULL;
		m_pvClientDC		= NULL;
		m_pvFrameWnd		= NULL;
		m_pvStatusBarWnd	= NULL;
		m_szOutBuff			= NULL;
		m_nOutBuffLen		= 0;
	}

	~CUI()
	{
		if (m_pvClientDC)
			delete (CClientDC *) m_pvClientDC;

		if (m_szOutBuff)
			delete [] m_szOutBuff;
	}


//#ifndef _CHAT
	// for ICommObject
//	STDMETHODIMP_(BOOL) Call( BOOL bSynchronous, WORD iCallId, PBYTE pbData, DWORD cb );
//#endif

	BOOL bAllocOutBuff(SHORT nLen)
	{
		if (m_szOutBuff)
			delete [] m_szOutBuff;

		if (m_szOutBuff = new CHAR[nLen])
		{
			m_nOutBuffLen = nLen;
			return TRUE;
		}
		else
		{
			m_nOutBuffLen = 0;
			return FALSE;
		}
	}

	// REVIEW: think about this
    void * GetCharSelBodyCamPv()
	{
		return m_pvCharSelBodyCamWnd;
	}
    void * GetStatusBarPv()
    {
        return m_pvStatusBarWnd;
    }
	void * GetToolBarPv()
	{
		return m_pvToolBarWnd;
	}
	void * GetClientDCPv()
	{
		return m_pvClientDC;
	}
	void * GetChatViewPv()
	{
		return m_pvChatView;
	}
	void * GetFramePv()
	{
		return m_pvFrameWnd;
	}
	void * GetRoomListPv()
	{
		return m_pvRoomList;
	}
	void * GetUserListPv()
	{
		return m_pvUserList;
	}
	void * GetChatDocPv()
	{
		return m_pvChatDoc;
	}

	void * GetPersonalPagePv()
	{
		return m_pvPersonalPage;
	}

	void * GetWhisperBoxPv()
	{
		return m_pvWhisperBox;
	}

	void * GetNotifBoxPv()
	{
		return m_pvNotifBox;
	}

	void * GetFocusedDocPv()
	{
		return m_pvFocusedDoc;
	}

	void * GetTabBarPv()
	{
		return m_pvTabBar;
	}

	void * GetIrcProtoPv()
	{
		return m_pvIrcProto;
	}

	void * GetStatusViewPv()
	{
		return m_pvStatusView;
	}

	char * GetOutBuffSz()
	{
		return m_szOutBuff;
	}

	short GetOutBuffLenN()
	{
		return m_nOutBuffLen;
	}
};

extern CUI cui;

// The original client shared a pair of fixed output buffers and wrote into them
// with unbounded CRT calls.  Measure the complete result before writing so an
// oversized user-controlled command is rejected instead of being truncated into
// a different, still-valid IRC command.
#if defined(__clang__) || defined(__GNUC__)
#define COMICCHAT_PRINTF_LIKE(formatIndex, argumentsIndex) \
	__attribute__((format(printf, formatIndex, argumentsIndex)))
#else
#define COMICCHAT_PRINTF_LIKE(formatIndex, argumentsIndex)
#endif

inline BOOL TryFormatBufferV(char *buffer, std::size_t capacity,
	const char *format, va_list arguments) COMICCHAT_PRINTF_LIKE(3, 0);
inline BOOL TryFormatBuffer(char *buffer, std::size_t capacity,
	const char *format, ...) COMICCHAT_PRINTF_LIKE(3, 4);

inline BOOL TryFormatBufferV(char *buffer, std::size_t capacity,
							 const char *format, va_list arguments)
{
	if (!buffer || !capacity || !format)
		return FALSE;

	buffer[0] = '\0';
	va_list measureArguments;
	va_copy(measureArguments, arguments);
	const int required = std::vsnprintf(NULL, 0, format, measureArguments);
	va_end(measureArguments);
	if (required < 0 || static_cast<std::size_t>(required) >= capacity)
		return FALSE;

	const int written = std::vsnprintf(buffer, capacity, format, arguments);
	if (written != required)
	{
		buffer[0] = '\0';
		return FALSE;
	}

	return TRUE;
}

inline BOOL TryFormatBuffer(char *buffer, std::size_t capacity,
						 const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const BOOL result = TryFormatBufferV(buffer, capacity, format, arguments);
	va_end(arguments);
	return result;
}

inline BOOL TryCopyBuffer(char *buffer, std::size_t capacity, const char *source)
{
	if (!buffer || !capacity || !source)
		return FALSE;

	const std::size_t length = std::strlen(source);
	if (length >= capacity)
	{
		buffer[0] = '\0';
		return FALSE;
	}

	std::memcpy(buffer, source, length + 1);
	return TRUE;
}

inline BOOL TryAppendBuffer(char *buffer, std::size_t capacity, const char *source)
{
	if (!buffer || !capacity || !source)
		return FALSE;

	const char *terminator = static_cast<const char *>(std::memchr(buffer, '\0', capacity));
	if (!terminator)
	{
		buffer[0] = '\0';
		return FALSE;
	}

	const std::size_t used = static_cast<std::size_t>(terminator - buffer);
	const std::size_t sourceLength = std::strlen(source);
	if (sourceLength >= capacity - used)
	{
		buffer[0] = '\0';
		return FALSE;
	}

	std::memcpy(buffer + used, source, sourceLength + 1);
	return TRUE;
}

#define TryFormatOutBuff(...) \
	TryFormatBuffer(GetOutBuff(), static_cast<std::size_t>(GetOutBuffLen()), __VA_ARGS__)
#define TryFormatArray(buffer, ...) \
	TryFormatBuffer((buffer), sizeof(buffer), __VA_ARGS__)
#define TryCopyArray(buffer, source) \
	TryCopyBuffer((buffer), sizeof(buffer), (source))
#define TryCopyOutBuff(source) \
	TryCopyBuffer(GetOutBuff(), static_cast<std::size_t>(GetOutBuffLen()), (source))

#undef COMICCHAT_PRINTF_LIKE

#endif // __UI_H__
