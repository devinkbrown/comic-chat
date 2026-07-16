//=--------------------------------------------------------------------------=
// IrcSock.Cpp:		Implementation of CIrcSocket C++ class
//=--------------------------------------------------------------------------=
// Copyright  1998  Microsoft Corporation.  All Rights Reserved.
//=--------------------------------------------------------------------------=

// Created by RegisB on 02/19/98

#include "stdafx.h"
#include "chatprot.h"
#include "chat.h"
#include "chatdoc.h"
#include "ircsock.h"
#include "ircproto.h"
#include "histent.h"
#include "setupdlg.h"
#include "ui.h"
#include "ccommon.h"
#include "format.h"
#include "status.h"
#include "motd.h"
#include "avatar.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

extern CChatApp theApp;
extern CPtrList g_docs;
extern BOOL		g_bCanViewUnrated;			// cached during room list

static CString		g_strBan;
static CStringArray g_arrayBans;

namespace {

constexpr std::size_t kMaximumIrcWireBytes = 8191 + 512;
constexpr std::size_t kMaximumEventBatch = 128;
constexpr std::size_t kMaximumEventBatchesPerWake = 8;
constexpr std::size_t kMaximumIrcv3AdapterEvents = 512;

void SecureClear(std::string* value)
{
	if (!value)
		return;
	volatile char* bytes = value->empty() ? nullptr : value->data();
	for (std::size_t index = 0; index < value->size(); ++index)
		bytes[index] = '\0';
	value->clear();
	value->shrink_to_fit();
}

} // namespace

#define SET_CMD(sz)		sz, (sizeof(sz) - 1)
//
// This table contains all supported IRC commands. MUST BE SORTED
// and MUST match the order of ID_CMD in ircsock.h
//
PRIRCCMD g_rgIrcCmd[]=
{
	SET_CMD("ACCESS"),	0x03, 0x00,
	SET_CMD("ACTION"),	0x02, 0x00,
	SET_CMD("AUTH"),	0x03, 0x00,
	SET_CMD("AWAY"),	0x02, 0x00,
	SET_CMD("CLONE"),	0x03, 0x00,
	SET_CMD("CREATE"),	0x02, 0x00,
	SET_CMD("DATA"),	0x02, 0x00,
	SET_CMD("ERROR"),	0x02, 0x00,
	SET_CMD("INFO"),	0x03, 0x00,
	SET_CMD("INVITE"),	0x02, 0x02,
	SET_CMD("ISON"),	0x03, 0x01,
	SET_CMD("JOIN"),	0x02, 0x00,
	SET_CMD("KICK"),	0x02, 0x02,
	SET_CMD("KILL"),	0x02, 0x01,
	SET_CMD("KILLED"),	0x02, 0x00,
	SET_CMD("KLINE"),	0x03, 0x00,
	SET_CMD("KNOCK"),	0x03, 0x00,
	SET_CMD("LIST"),	0x02, 0x00,
	SET_CMD("LISTX"),	0x03, 0x00,
	SET_CMD("LUSERS"),	0x03, 0x00,
	SET_CMD("ME"),		0x02, 0x00,
	SET_CMD("MODE"),	0x02, 0x01,
	SET_CMD("MSG"),		0x02, 0x00,
	SET_CMD("NAMES"),	0x03, 0x00,
	SET_CMD("NICK"),	0x02, 0x01,
	SET_CMD("NOTICE"),	0x02, 0x00,
	SET_CMD("PART"),	0x02, 0x00,
	SET_CMD("PASS"),	0x02, 0x00,
	SET_CMD("PING"),	0x03, 0x00,
	SET_CMD("PONG"),	0x02, 0x00,
	SET_CMD("PRIVMSG"),	0x02, 0x00,
	SET_CMD("PROP"),	0x02, 0x00,
	SET_CMD("QUIT"),	0x02, 0x00,
	SET_CMD("QUOTE"),	0x03, 0x00,
	SET_CMD("RAW"),		0x03, 0x00,
	SET_CMD("REPLY"),	0x02, 0x00,
	SET_CMD("REQUEST"),	0x02, 0x00,
	SET_CMD("SERVER"),	0x01, 0x00,
	SET_CMD("SOUND"),	0x02, 0x00,
	SET_CMD("THINK"),	0x02, 0x00,
	SET_CMD("TOPIC"),	0x03, 0x01,
	SET_CMD("UNKLINE"),	0x03, 0x00,
	SET_CMD("USER"),	0x03, 0x00,
	SET_CMD("USERHOST"),0x03, 0x01,
	SET_CMD("WHISPER"),	0x02, 0x00,
	SET_CMD("WHO"),		0x02, 0x00,
	SET_CMD("WHOIS"),	0x03, 0x01
};

extern void ChatChangeAdmin(CChatDoc *doc, const char *szNickname, int sets, int unsets);
extern void IgnoreUser(const char *, const char *, BOOL, BOOL);
extern void ShowIdentity(const char *nick, const char *user, const char *host);


SHORT NGetCmd(LPCSTR szCmd)
{
	ASSERT(szCmd);
	//
	// binary search the command table
	//
	SHORT	nMiddle;
	SHORT	nStart, nEnd;	// search range
	SHORT	nRet;

	nStart	= 0;
	nEnd	= cmdidMax - 1;

	do
	{
		nMiddle = (nEnd - nStart)/2 + nStart;

		nRet = ::lstrcmpi(szCmd, g_rgIrcCmd[nMiddle].szCmd);
		if (0 == nRet) // a match
			return nMiddle;

		if (nStart == nEnd)
			break;

		if (-1 == nRet)
		{
			//
			// The cmd is less than
			//
			nEnd = nMiddle;
		}
		else
		{
			if (nMiddle != nStart)
				nStart = nMiddle;
			else
				nStart = nEnd;
		}
	}
	while (TRUE);

	return -1;	// not found
}


void ParseIt(const char *szMessage, PIRCPARSE pParse, BOOL bDoubleQuotes /*=FALSE*/)
{
	// parse prefix
	const char	*szStart = szMessage;
	char		*szBody, *szCurToken;
	char		szPrefixBuff[300];

	pParse->nick[0] = '\0';
	pParse->user[0] = '\0';
	pParse->machine[0] = '\0';
	pParse->lastString = NULL;
	pParse->uCode = 0;
	pParse->nArgs = 0;
	ZeroMemory(pParse->args, sizeof(CHAR*) * MAXARGS);
	ZeroMemory(pParse->nOffsets, sizeof(SHORT) * MAXARGS);

	if (*szMessage == ':')
	{	// there's a prefix
		szMessage++;					// don't include the colon
		pParse->bHasPrefix = TRUE;
		szBody = (char *)strchr(szMessage, ' ');
		ASSERT(szBody);					// messages must have a body
		int cbPrefixSize = szBody - szMessage;
		cbPrefixSize = min(cbPrefixSize, sizeof(szPrefixBuff)-1);
		strncpy(szPrefixBuff, szMessage, cbPrefixSize);
		szPrefixBuff[cbPrefixSize] = '\0';
		char *szStart = szPrefixBuff;
		char *szEnd = NULL;
		if (!CHANNELPREFIX(*szStart)) szEnd = strpbrk(szStart, "!@");	// parse snick (must be present)
		if (szEnd)
		{
			int nChars = szEnd - szStart;
			nChars = min(nChars, sizeof(pParse->nick)-1);
			strncpy(pParse->nick, szStart, nChars);
			pParse->nick[nChars] = '\0';
			if (*szEnd == '!')
			{	// parse user
				szStart = szEnd+1;
				szEnd = (char *)strchr(szStart, '@');
				if (szEnd)
				{
					int nChars = szEnd - szStart;
					nChars = min(nChars, sizeof(pParse->user)-1);
					strncpy(pParse->user, szStart, nChars);
					pParse->user[nChars] = '\0';
					nChars = sizeof(pParse->machine)-1;
					strncpy(pParse->machine, szEnd+1, nChars);			// nfield now pts to !, parse machine
					pParse->machine[nChars] = '\0';
				}
			}
		}
		else
			if (strlen(szPrefixBuff) < sizeof(pParse->nick))
				TryCopyArray(pParse->nick, szPrefixBuff);
	}
	else
	{
		pParse->bHasPrefix = FALSE;
		szBody = UnConst(szMessage);
	}

	while (TRUE)
	{
		while (my_isspace(*szBody))
			szBody++;
		if (*szBody == ':')
		{
			szBody++;
end:		char *szEnd = strpbrk(szBody, "\r\n");
			if (!szEnd)
				szEnd = (char *)strchr(szBody, '\0');
			int cbLen = szEnd - szBody;
			if (pParse->lastString = (char*) malloc(cbLen+1))
			{
				strncpy(pParse->lastString, szBody, cbLen);
				pParse->lastString[cbLen] = '\0';
			}
			break;
		}
		char *szToken;
		if (bDoubleQuotes && *szBody == '\"')
		{
			szToken = GetToken1(szBody, &szBody, "\"\r\n", &szCurToken, FALSE /*bSkipInitialSeps*/);
			if (*szBody == '\"')	// skip the terminating double quote
			{
				szBody++;
				TryAppendBuffer(szToken, MAX_TOKEN, "\"");
			}
		}
		else
		{
			szToken = GetToken(szBody, &szBody, " \r\n", &szCurToken);
			if (!szToken)
				break;
		}
		pParse->nOffsets[pParse->nArgs] = szCurToken - szStart;
		pParse->args[pParse->nArgs++] = strdup(szToken);
		if (pParse->nArgs == MAXARGS)
		{
			TRACE("Too many parameters - Have to use lastString for remaining message.\n");
			goto end;
		}
	}

	// Now fill in uCode member
	if (pParse->args[0])
	{
		CHAR	ch = pParse->args[0][0];
		INT		i  = 0;
 		if (isdigit(ch))
		{
			// Result or Error Code
			do
			{
				pParse->uCode *= 10;
				pParse->uCode += (ch - '0');
				ch = pParse->args[0][++i];
			}
			while (isdigit(ch));
		}
	}
}


void FreeParse(PIRCPARSE pParse)
{
	if (pParse->lastString)
		free(pParse->lastString);
	for (SHORT i = 0; i < pParse->nArgs; i++)
		free(pParse->args[i]);
}


BOOL bSingleJoin(char *szAttedNick, void *pDoc, DWORD dwData)
{
	ASSERT(pDoc);
	CUserInfo* pui;
	AddAndExecute(new JoinEntry(pui = new CUserInfo(szAttedNick)), (CDocument *) pDoc);
	return pui == ((CChatDoc*) pDoc)->m_puiSelf;
}


void CSInString(char **pszString, const char *szChannelName = NULL, CChatDoc *doc = NULL) {
	ASSERT(*pszString);
	int iEncoding = (szChannelName && *szChannelName == '%') ? ENC_UTF8 : ENC_DBCS;
	if (doc && (doc->m_proto->m_dwModes & CM_MIC)) iEncoding = ENC_DBCS; // until MIC disappears
	if (!**pszString || (theApp.m_charSet == ANSI_CHARSET && iEncoding == ENC_DBCS)) return;
	char *szNewString = strdup(DecodeString(*pszString, iEncoding));
	free(*pszString);
	*pszString = szNewString;
}


void CSInPlace(char *szNick, std::size_t capacity) {
	if (theApp.m_charSet == ANSI_CHARSET || *szNick == '\0') return;;
	char *szOldNick = strdup(szNick);
	CSInString(&szOldNick);
	TryCopyBuffer(szNick, capacity, szOldNick);
	free(szOldNick);
}


void ParseChannelMode(CChatDoc *doc, const char *szFlags, const char *szArg2, const char *szArg3, CRoomInfo *pEnterRoom)
{
	ASSERT(doc);

	BOOL	bAdd = TRUE;
	DWORD	dwDelta = 0, addFlags = 0, subFlags = 0;

	while (*szFlags != '\0')
	{
		switch (*szFlags++)
		{
		case '+':
			bAdd = TRUE;
			dwDelta = 0;
			break;
		case '-':
			bAdd = FALSE;
			dwDelta = 0;
			break;
		case 'p':
			dwDelta |= CM_PRIVATE;
			break;
		case 's':
			dwDelta |= CM_HIDDEN;
			break;
		case 'i':
			dwDelta |= CM_INVITEONLY;
			break;
		case 't':
			dwDelta |= CM_TOPICHOST;
			break;
		case 'n':
			dwDelta |= CM_NOEXTERN;
			break;
		case 'm':
			dwDelta |= CM_MODERATED;
			break;
		case 'l':
			dwDelta |= CM_USERLIMIT;
			doc->m_proto->m_dwMaxUsers = bAdd ? atoi(szArg2) : 0;
			break;
		case 'k':
			dwDelta |= CM_CHANNELKEY;
			if (bAdd)
				doc->m_proto->m_strPassword = (szArg3 && *szArg3) ? szArg3 : szArg2;
			else
				doc->m_proto->m_strPassword = ""; // clear it out, since we check for changes in the value before setting
			break;
		case 'q':
		    if (bAdd)
				ChatChangeAdmin(doc, szArg2, UF_OWNER | UF_OPERATOR, 0);
			else
				ChatChangeAdmin(doc, szArg2, 0, UF_OWNER);
			break;
		case 'o':
			if (bAdd)
				ChatChangeAdmin(doc, szArg2, UF_OPERATOR, 0);
			else
				ChatChangeAdmin(doc, szArg2, 0, UF_OPERATOR);
			break;
		case 'v':
			if (bAdd)
				ChatChangeAdmin(doc, szArg2, UF_HASVOICE, 0);
			else
				ChatChangeAdmin(doc, szArg2, 0, UF_HASVOICE);
			break;
		case 'f':
			if (GetIrcProto()->IsIRCX())
			{   // +f means something else for non-IRCX servers
				dwDelta |= CM_NOFORMAT;
				if (bAdd)
				{
					theApp.m_bSaveViewMode = FALSE;
					doc->OnViewText();
				}
			}
			break;
		case 'y':
			dwDelta |= CM_MIC;
			if (bAdd && pEnterRoom)
				FixMICChannelName(doc, pEnterRoom);
			break;
		}
		if (bAdd)
			addFlags |= dwDelta;
		else
			subFlags |= dwDelta;
	}

	doc->m_proto->m_dwModes |= addFlags;
	doc->m_proto->m_dwModes &= ~subFlags;

	void UpdateSpectators(CChatDoc *doc, BOOL moderated);
	if ((addFlags | subFlags) & CM_MODERATED)
	{
		UpdateSpectators(doc, doc->m_proto->m_dwModes & CM_MODERATED);
		// REGISB 05/04/98 - Bug 2459 - code ready to be checked in if we decide to
		// send a # Appears as when channel becomes non-moderated.
		// if (subFlags & CM_MODERATED)
		// 	doc->m_proto->ChatAnnounceNewAvatar(GetMyCharacter(), MyAvatarURL());  // announce avatar to room when the moderated flag goes away
	}
}


void GetBanString(const char *szUserName, const char *szHostName, CString& strBan) {
	ASSERT(szUserName);
	ASSERT(szHostName);
	if (!GetIrcProto()->IsIRCX() || *szUserName == '~')
		strBan.Format("*!*@%s", szHostName);  // not authenticated
	else
		strBan.Format("*!%s@%s", szUserName, szHostName);
}


CIrcSocket::CIrcSocket()
	: m_wakeupState(std::make_shared<WakeupState>())
{
	m_nMaxMsgLength = 0;
	m_iConnected = CX_DISCONNECTED;
	m_nAuthenticationType = authtypeNone;	// No authentication by default
	Reset();
}


CIrcSocket::~CIrcSocket()
{
	Close();

	SecureClear(&m_userName);
	m_password.clear();
}


BOOL CIrcSocket::StorePassword(std::string_view password)
{
	if (password.empty()) {
		m_password.clear();
		return TRUE;
	}
	auto locked = comicchat::LockedSecret::copy(password);
	if (!locked) {
		m_password.clear();
		return FALSE;
	}
	m_password = std::move(*locked);
	return TRUE;
}


BOOL CIrcSocket::CopyPassword(std::string* password) const
{
	if (!password)
		return FALSE;
	SecureClear(password);
	const auto bytes = m_password.view();
	try {
		password->assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	} catch (const std::bad_alloc&) {
		SecureClear(password);
		return FALSE;
	}
	return TRUE;
}


BOOL CIrcSocket::HasPassword() const
{
	return !m_password.view().empty();
}


void CIrcSocket::Reset(void)
{
	m_bIrcXServer	= FALSE;
	m_bRegistered	= FALSE;
	m_bAnonAllowed	= FALSE;
	m_bJustSentModeIsIrcX = FALSE;
	m_bLoginPending = FALSE;
	m_lineFramer.Reset();
}


HRESULT CIrcSocket::HrInitAlloc(SHORT nMaxIOBuff)
{
	if (nMaxIOBuff <= 0)
		return E_INVALIDARG;

	try
	{
		m_outputBuffer.assign(static_cast<std::size_t>(nMaxIOBuff) + 1, '\0');
	}
	catch (const std::bad_alloc&)
	{
		m_nMaxMsgLength = 0;
		return E_OUTOFMEMORY;
	}

	if (cui.bAllocOutBuff((MAX_FORMATTINGPERBYTE+1)*MAX_INPUTLEN+MAX_COMMAND))
	{
		m_nMaxMsgLength	= nMaxIOBuff;
		return NOERROR;
	}
	else
	{
		m_nMaxMsgLength	= 0;
		return E_OUTOFMEMORY;
	}
}

BOOL CIrcSocket::FormatOutput(LPCSTR pszFormat, ...)
{
	if (m_outputBuffer.empty())
		return FALSE;

	va_list arguments;
	va_start(arguments, pszFormat);
	const BOOL result = TryFormatBufferV(
		m_outputBuffer.data(), m_outputBuffer.size(), pszFormat, arguments);
	va_end(arguments);
	return result;
}

CHAR* CIrcSocket::GetOutput()
{
	return m_outputBuffer.empty() ? NULL : m_outputBuffer.data();
}

BOOL CIrcSocket::Connect(LPCSTR pszServer, UINT nPort, BOOL bSecure)
{
	return StartConnection(pszServer, nPort, bSecure).has_value();
}

std::expected<comicchat::net::GenerationId, CIrcSocket::AdapterError>
CIrcSocket::StartConnection(LPCSTR pszServer, UINT nPort, BOOL bSecure)
{
	if (!pszServer || !*pszServer || nPort == 0 || nPort > 65535)
		return std::unexpected(AdapterError::transport_error);

	// Copy before Close(): an STS upgrade passes m_serverHost.c_str() back into
	// this method and the member must remain valid throughout the restart.
	std::string serverHost = pszServer;
	Close();
	comicchat::net::ConnectionOptions options;
	options.endpoint.host = serverHost;
	options.endpoint.port = static_cast<std::uint16_t>(nPort);
	options.security = bSecure ? comicchat::net::Security::tls : comicchat::net::Security::plaintext;
	options.server_name = serverHost;
	options.limits.receive_bytes = 256U * 1024U;
	options.limits.transmit_bytes = 256U * 1024U;
	options.limits.queued_commands = 1024;
	static std::atomic<DWORD> nextWakeupCookie{1};
	DWORD cookie = nextWakeupCookie.fetch_add(1, std::memory_order_relaxed);
	if (cookie == 0)
		cookie = nextWakeupCookie.fetch_add(1, std::memory_order_relaxed);
	const HWND hwnd = AfxGetMainWnd() ? AfxGetMainWnd()->GetSafeHwnd() : NULL;
	m_wakeupState->hwnd.store(hwnd, std::memory_order_release);
	m_wakeupState->pending.store(false, std::memory_order_release);
	m_wakeupState->cookie.store(cookie, std::memory_order_release);
	m_connection.set_wakeup([weak = std::weak_ptr<WakeupState>(m_wakeupState), cookie]() {
		const auto state = weak.lock();
		if (!state || state->cookie.load(std::memory_order_acquire) != cookie)
			return;
		const HWND target = state->hwnd.load(std::memory_order_acquire);
		if (!target || state->pending.exchange(true, std::memory_order_acq_rel))
			return;
		if (state->cookie.load(std::memory_order_acquire) != cookie ||
			state->hwnd.load(std::memory_order_acquire) != target ||
			!::PostMessage(target, WM_COMICCHAT_NETWORK_EVENT, 0, static_cast<LPARAM>(cookie)))
			state->pending.store(false, std::memory_order_release);
	});
	auto generation = m_connection.start(std::move(options));
	if (!generation) {
		Close();
		return std::unexpected(AdapterError::transport_error);
	}
	m_serverHost = std::move(serverHost);
	m_generation = *generation;
	m_bSecureTransport = bSecure;
	m_bTransportOpen = TRUE;
	m_transportState = comicchat::net::State::resolving;
	return m_generation;
}

void CIrcSocket::Close()
{
	m_connection.set_wakeup({});
	m_wakeupState->hwnd.store(NULL, std::memory_order_release);
	m_wakeupState->cookie.store(0, std::memory_order_release);
	m_wakeupState->pending.store(false, std::memory_order_release);
	m_connection.stop();
	m_bTransportOpen = FALSE;
	m_generation = 0;
	m_localAddress.clear();
	m_transportState = comicchat::net::State::stopped;
	m_lineFramer.Reset();
}

BOOL CIrcSocket::IsOpen() const
{
	return m_bTransportOpen;
}

comicchat::net::Priority CIrcSocket::PriorityFor(std::string_view wire) const
{
	auto parsed = comic_chat::ircv3::Message::Parse(wire);
	if (!parsed)
		return comicchat::net::Priority::bulk;
	if (parsed->command == "AUTHENTICATE" || parsed->command == "PASS")
		return comicchat::net::Priority::authentication;
	if (parsed->command == "PING" || parsed->command == "PONG")
		return comicchat::net::Priority::pong;
	if (parsed->command == "CAP" || parsed->command == "NICK" || parsed->command == "USER" ||
		parsed->command == "QUIT" || parsed->command == "MODE")
		return comicchat::net::Priority::control;
	if (parsed->command == "PRIVMSG" || parsed->command == "NOTICE")
		return comicchat::net::Priority::chat;
	return comicchat::net::Priority::bulk;
}

BOOL CIrcSocket::IsSensitive(std::string_view wire) const
{
	auto parsed = comic_chat::ircv3::Message::Parse(wire);
	if (!parsed)
		return TRUE;
	return parsed->command == "AUTHENTICATE" || parsed->command == "PASS" ||
		parsed->command == "AUTH" || parsed->command == "OPER" ||
		parsed->command == "REGISTER" || parsed->command == "VERIFY";
}

int CIrcSocket::Send(void* pData, int nBytes)
{
	if (!pData || nBytes <= 0)
		return SOCKET_ERROR;
	const auto result = QueueProtocolLine(
		std::string_view(static_cast<const char*>(pData), static_cast<std::size_t>(nBytes)));
	if (IsSensitive(std::string_view(static_cast<const char*>(pData), static_cast<std::size_t>(nBytes)))) {
		volatile char* bytes = static_cast<char*>(pData);
		for (int index = 0; index < nBytes; ++index) bytes[index] = '\0';
	}
	return result ? nBytes : SOCKET_ERROR;
}

std::expected<comicchat::net::SendId, CIrcSocket::AdapterError>
CIrcSocket::QueueProtocolLine(std::string_view wire)
{
	if (!m_bTransportOpen)
		return std::unexpected(AdapterError::not_open);
	if (wire.size() > kMaximumIrcWireBytes)
		return std::unexpected(AdapterError::line_too_long);
	auto prepared = m_ircEngine.PrepareOutgoingChecked(wire);
	if (!prepared)
		return std::unexpected(AdapterError::invalid_line);

	comicchat::net::Send command;
	command.generation = m_generation;
	const auto sendId = m_nextSendId++;
	command.id = sendId;
	command.priority = PriorityFor(*prepared);
	command.sensitive = IsSensitive(*prepared);
	if (const auto parsed = comic_chat::ircv3::Message::Parse(*prepared);
		parsed && (parsed->command == "PRIVMSG" || parsed->command == "NOTICE" ||
			parsed->command == "TAGMSG") && !parsed->params.empty())
		command.target = parsed->params.front();
	command.bytes.reserve(prepared->size());
	for (const unsigned char byte : *prepared)
		command.bytes.push_back(static_cast<std::byte>(byte));
	if (command.sensitive) SecureClear(&*prepared);
	if (!m_connection.post(std::move(command)))
		return std::unexpected(AdapterError::transport_error);
	return sendId;
}

void CIrcSocket::DispatchProtocolMessage(const comic_chat::ircv3::Message& message)
{
	if (!message.tags.empty()) {
		comic_chat::ircv3::Event context;
		context.type = comic_chat::ircv3::EventType::MessageContext;
		context.source = message.prefix ? *message.prefix : std::string{};
		context.target = message.params.empty() ? std::string{} : message.params.front();
		context.key = message.command;
		DispatchProtocolEvent(std::move(context), message);
	}
	std::string wire = message.Serialize(false);
	std::vector<char> dispatchBuffer(wire.begin(), wire.end());
	dispatchBuffer.push_back('\0');
	ProcessMessage(dispatchBuffer.data());
}

void CIrcSocket::DispatchProtocolEvent(comic_chat::ircv3::Event event,
	std::optional<comic_chat::ircv3::Message> message)
{
	if (m_ircv3Events.size() >= kMaximumIrcv3AdapterEvents) {
		m_ircv3Events.pop_front();
		++m_droppedIrcv3Events;
	}
	m_ircv3Events.push_back({std::move(event), std::move(message)});
}

std::vector<Ircv3AdapterEvent> CIrcSocket::PollIrcv3Events(std::size_t maximum)
{
	std::vector<Ircv3AdapterEvent> events;
	events.reserve((std::min)(maximum, m_ircv3Events.size()));
	while (!m_ircv3Events.empty() && events.size() < maximum) {
		events.push_back(std::move(m_ircv3Events.front()));
		m_ircv3Events.pop_front();
	}
	return events;
}

void CIrcSocket::PollNetworkEvents(LPARAM wakeupCookie)
{
	const auto cookie = static_cast<DWORD>(wakeupCookie);
	if (cookie == 0 || m_wakeupState->cookie.load(std::memory_order_acquire) != cookie)
		return;
	m_wakeupState->pending.store(false, std::memory_order_release);
	bool possiblyMore = false;
	for (std::size_t batch = 0; batch < kMaximumEventBatchesPerWake; ++batch) {
		auto events = m_connection.poll_events(kMaximumEventBatch);
		if (events.empty()) {
			possiblyMore = false;
			break;
		}
		possiblyMore = events.size() == kMaximumEventBatch;
		for (auto& event : events) {
			if (event.generation != m_generation)
				continue;
			std::visit([this](auto&& body) {
			using Body = std::remove_cvref_t<decltype(body)>;
			if constexpr (std::is_same_v<Body, comicchat::net::StateChanged>) {
				m_transportState = body.state;
			} else if constexpr (std::is_same_v<Body, comicchat::net::Connected>) {
				m_localAddress = body.local_address;
				m_transportState = comicchat::net::State::connected;
				m_bSecureTransport = body.tls;
				m_bTransportOpen = TRUE;
				if (AfxGetMainWnd())
					AfxGetMainWnd()->SendMessage(WM_COMMAND, ID_CONNECT_CONNECTED, 0);
				OnConnect(0);
			} else if constexpr (std::is_same_v<Body, comicchat::net::BytesReceived>) {
				if (!body.bytes)
					return;
				auto lines = m_lineFramer.Push(std::span<const std::byte>(*body.bytes));
				if (!lines) {
					TRACE0("IRC transport rejected an invalid or oversized frame.\n");
					OnClose(WSAEMSGSIZE);
					return;
				}
				for (const auto& line : *lines) {
					auto result = m_ircEngine.Process(line);
					// STS discovered on plaintext must be applied before any CAP or
					// SASL response produced by this line is allowed onto the wire.
					// The secure connection reruns capability negotiation from zero.
					if (!m_bSecureTransport) {
						const auto stsEvent = std::find_if(result.events.begin(), result.events.end(),
							[](const comic_chat::ircv3::Event& event) {
								return event.type == comic_chat::ircv3::EventType::StsPolicy &&
									event.key == "upgrade";
							});
						const auto& policy = m_ircEngine.CurrentStsPolicy();
						if (stsEvent != result.events.end() && policy && policy->port && !m_serverHost.empty()) {
							const std::string host = m_serverHost;
							if (!StartConnection(host.c_str(), *policy->port, TRUE))
								OnClose(WSAECONNABORTED);
							return;
						}
					}
					for (auto& protocolEvent : result.events)
						DispatchProtocolEvent(std::move(protocolEvent));
					for (const auto& outbound : result.outbound) {
						if (!QueueProtocolLine(outbound)) {
							TRACE0("IRC protocol control message could not be queued; closing connection.\n");
							OnClose(WSAENOBUFS);
							return;
						}
					}
					for (const auto& message : result.messages)
						DispatchProtocolMessage(message);
					if (m_bLoginPending && m_ircEngine.RegistrationFinished()) {
						m_bLoginPending = FALSE;
						(void)HrIrcXLogin(FALSE);
					}
				}
			} else if constexpr (std::is_same_v<Body, comicchat::net::Closed>) {
				m_transportState = body.retry_after.count() > 0
					? comicchat::net::State::reconnect_wait
					: comicchat::net::State::stopped;
				if (m_bTransportOpen) {
					m_bTransportOpen = FALSE;
					m_localAddress.clear();
					OnClose(WSAECONNRESET);
				}
			} else if constexpr (std::is_same_v<Body, comicchat::net::Diagnostic>) {
				TRACE("IRC transport diagnostic [%s].\n", body.code.c_str());
			} else if constexpr (std::is_same_v<Body, comicchat::net::PingDue>) {
				auto ping = m_ircEngine.PrepareKeepalivePing();
				if (!ping || !QueueProtocolLine(*ping)) {
					TRACE0("IRC keepalive PONG deadline expired.\n");
					OnClose(WSAETIMEDOUT);
				}
			}
			}, event.body);
		}
		if (events.size() < kMaximumEventBatch)
			break;
	}
	if (possiblyMore && m_wakeupState->cookie.load(std::memory_order_acquire) == cookie) {
		const HWND target = m_wakeupState->hwnd.load(std::memory_order_acquire);
		if (target && !m_wakeupState->pending.exchange(true, std::memory_order_acq_rel) &&
			!::PostMessage(target, WM_COMICCHAT_NETWORK_EVENT, 0, static_cast<LPARAM>(cookie)))
			m_wakeupState->pending.store(false, std::memory_order_release);
	}
}

BOOL
CIrcSocket::PromptForPassword(
LPCSTR pszUserName,
BOOL bSaveInSettings)
{
	(void)bSaveInSettings;
	CChatPasswordDialog dlg (GetMyPhysicalServer (), pszUserName, FALSE);
	if (theApp.DoModalDlg (&dlg) == IDOK)
	{
		const int length = dlg.m_strPassword.GetLength();
		LPSTR password = dlg.m_strPassword.GetBuffer(length);
		const BOOL stored = StorePassword(std::string_view(
			password ? password : "", static_cast<std::size_t>(std::max(length, 0))));
		if (password && length > 0)
			SecureZeroMemory(password, static_cast<std::size_t>(length));
		dlg.m_strPassword.ReleaseBuffer(0);
		return stored;
	}
	else
	{
		return FALSE;
	}
}


HRESULT CIrcSocket::HrModeIsIrcXFailure()
{
	HRESULT hr = NOERROR;

	if (m_bJustSentModeIsIrcX)
	{
		for (const auto& command : m_ircEngine.FinishRegistrationAfterTimeout()) {
			if (!QueueProtocolLine(command)) {
				OnClose(WSAENOBUFS);
				return HRESULT_FROM_WIN32(WSAENOBUFS);
			}
		}
		POSITION	pos;
		CCQuery*	pQuery = m_queries.FindQuery(ctModeIsIrcX, &pos);

		if (pQuery)
		{
			ASSERT(pos);
			m_queries.FreeRemoveAt(pos);
		}

		// Don't want to expose this error to the user, it comes from the MODE ISIRCX\r\n command on an IRC server
		ASSERT(m_bIrcXServer == FALSE);
		hr = HrIrcXLogin(FALSE);
		m_bLoginPending = FALSE;
		m_bJustSentModeIsIrcX = FALSE;
		::AfxGetMainWnd()->KillTimer(ID_ISIRCXTIMEOUT);
	}
	return hr;
}


//
// The account password is used only by SASL. This method emits the ordinary
// nickname and USER registration messages after capability negotiation.
//
HRESULT
CIrcSocket::HrIrcLogin(
BOOL bIRCX,
LPCSTR szNickname,
LPCSTR szUserName,
LPCSTR szRealName,
LPCSTR szPassword,
BOOL bPromptForPassword)
{
	ASSERT(szNickname);
	(void)szPassword;
	(void)bPromptForPassword;

	if (szUserName == NULL)
	{
		szUserName = GetMyUserName ();
	}

	std::string user;
	user.reserve(63);
	for (LPCSTR source = (szUserName && *szUserName) ? szUserName : szNickname;
		*source && user.size() < 63; ++source) {
		if (IsDBCSLeadByte(*source) && source[1] && user.size() + 2 <= 63) {
			user.push_back(*source++);
			user.push_back(*source);
		} else if (*source != ' ') {
			user.push_back(*source);
		}
	}

	// need to add EncodeString calls...

	// Account credentials are handled only by the shared SASL engine. They
	// must never be reused as a clear-text IRC PASS value.
	GetIrcProto()->ChatChangeNick(szNickname);

	if (!m_bRegistered)
	{
		// RFC 2812 registration does not need to expose the local host name.
		const std::size_t capacity = std::min(
			static_cast<std::size_t>(GetOutBuffLen()),
			static_cast<std::size_t>(m_nMaxMsgLength) + 1);
		if (!TryFormatBuffer(GetOutBuff(), capacity, "USER %s 0 * :%s\r\n",
				user.c_str(), szRealName ? szRealName : user.c_str()))
			return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
		GetIrcProto()->SendMessageText(GetOutBuff());

		// From now on we are registered until we disconnect
		m_bRegistered = TRUE;
	}

	(void)bIRCX;

	return NOERROR;
}


HRESULT CIrcSocket::HrIrcXLogin(BOOL)
{
	if (!m_ircEngine.RegistrationFinished()) {
		m_bLoginPending = TRUE;
		return S_FALSE;
	}

	if (m_ircEngine.SaslSucceeded() || m_bAnonAllowed ||
		m_nAuthenticationType == authtypeNone || m_nAuthenticationType == authtypePlainText) {
		return HrIrcLogin(TRUE, GetMyName(), m_userName.empty() ? NULL : m_userName.c_str(),
			GetMyRealName(), NULL, FALSE);
	}

	AfxMessageBox(ID_ERR_NOAUTH);
	return E_ACCESSDENIED;
}
HRESULT CIrcSocket::HrIrcSetOper(LPCSTR szUserName, LPCSTR szPassword)
{
	comicchat::LockedSecret promptedPassword;
	std::string materializedPassword;
	if (szPassword == NULL || *szPassword == '\0')
	{
		CChatPasswordDialog dlg(GetMyPhysicalServer(), szUserName, FALSE);
		if (theApp.DoModalDlg(&dlg) != IDOK)
			return S_FALSE;
		const int length = dlg.m_strPassword.GetLength();
		LPSTR password = dlg.m_strPassword.GetBuffer(length);
		auto locked = comicchat::LockedSecret::copy(std::string_view(
			password ? password : "", static_cast<std::size_t>(std::max(length, 0))));
		if (password && length > 0)
			SecureZeroMemory(password, static_cast<std::size_t>(length));
		dlg.m_strPassword.ReleaseBuffer(0);
		if (!locked)
			return E_OUTOFMEMORY;
		promptedPassword = std::move(*locked);
		const auto bytes = promptedPassword.view();
		try {
			materializedPassword.assign(
				reinterpret_cast<const char*>(bytes.data()), bytes.size());
		} catch (const std::bad_alloc&) {
			promptedPassword.clear();
			return E_OUTOFMEMORY;
		}
		szPassword = materializedPassword.c_str();
	}
	if (!szPassword || !*szPassword)
		return E_INVALIDARG;

	const std::size_t capacity = std::min(
		static_cast<std::size_t>(GetOutBuffLen()),
		static_cast<std::size_t>(m_nMaxMsgLength) + 1);
	if (!TryFormatBuffer(GetOutBuff(), capacity, "OPER %s %s\r\n", szUserName, szPassword)) {
		SecureClear(&materializedPassword);
		promptedPassword.clear();
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	GetIrcProto()->SendMessageText(GetOutBuff ());
	SecureClear(&materializedPassword);
	promptedPassword.clear();

	return NOERROR;
}


void CIrcSocket::OnConnect(int nErrorCode) {
	TRACE("IRC transport connection result: %d.\n", nErrorCode);
	if (nErrorCode) {  // couldn't connect
		GetIrcProto()->SetConnectionStatus(CX_DISCONNECTED);
		CString strMesg;
		strMesg.LoadString(ID_ERR_CONNECT);
//		setupDlg.nWhatFailed = PORT;
		VERIFY(ReplaceToken(strMesg, CString("%1"), GetMyServer()));
		AfxMessageBox(strMesg);
		InitializeServerConnection(&g_enterInfo, &g_bCXPrompt);
		return;
	}
	// got a connection!
	if (m_nAuthenticationType != authtypeNone && !HasPassword() &&
		!PromptForPassword(m_userName.empty() ? GetMyUserName() : m_userName.c_str(), TRUE)) {
		TRACE0("IRC account password was not supplied or could not be locked.\n");
		OnClose(ERROR_CANCELLED);
		return;
	}
	comic_chat::ircv3::SaslConfig sasl;
	if (!m_userName.empty())
		sasl.authentication_id = m_userName;
	else if (HasPassword())
		sasl.authentication_id = GetMyUserName();
	if (!CopyPassword(&sasl.password)) {
		OnClose(ERROR_OUTOFMEMORY);
		return;
	}
	// EXTERNAL is only valid when a client certificate is configured. The
	// legacy settings do not expose one, so never claim it implicitly.
	sasl.allow_external = false;
	auto commands = m_ircEngine.BeginRegistration(std::move(sasl), GetMyName(), m_bSecureTransport != FALSE);
	m_bLoginPending = TRUE;
	SecureClear(&sasl.authentication_id);
	SecureClear(&sasl.password);
	SecureClear(&sasl.authorization_id);
	for (const auto& command : commands) {
		if (!QueueProtocolLine(command)) {
			TRACE0("IRC capability negotiation message could not be queued; closing connection.\n");
			OnClose(WSAENOBUFS);
			return;
		}
	}

	// Is this an IRCX server?
	ASSERT(GetIrcProto());
	VERIFY(GetIrcProto()->bExecuteQuery(qpIsIrcX, ctModeIsIrcX, dtMax, NULL, "", ""));
	m_bJustSentModeIsIrcX = TRUE;
	::AfxGetMainWnd()->SetTimer(ID_ISIRCXTIMEOUT, ISIRCXTIMEOUT, NULL);
}


void CIrcSocket::OnClose(int nErrorCode)
{
	TRACE("IRC transport closed with status %d.\n", nErrorCode);
	ChatServerDisconnect(TRUE /*bCheckRules*/);
	AfxMessageBox(IDS_CONNECTION_DROPPED);
}


void CIrcProto::OnLogin()
{
	CChatService* AddToServerList(const char *);

	if (theApp.m_dynaRules.bDaemonNeeded())
		VERIFY(theApp.m_dynaRules.bStartRulesDaemon(g_uRulesDaemonShortElapse, TRUE /*bForceReset*/));

	if (theApp.m_dynaNotifs.bDaemonNeeded())
		VERIFY(theApp.m_dynaNotifs.bStartNotifsDaemon(g_uNotifsDaemonShortElapse, TRUE /*bForceReset*/));

	AddToServerList(GetMyServer());
	theApp.m_bInSearch = FALSE;			// can now search again (in case disconnected during last search
	SetVisibility(theApp.m_flags1 & F1_USERVISIBLE);

	int iAction;
	if (theApp.m_bLoadURL)
	{
		iAction = CA_JOINROOM;
		theApp.m_bLoadURL = FALSE;	// Don't do this on a reconnect.
	}
	else
		iAction = theApp.m_iOnConnectAction;
	switch (iAction)
	{
	case CA_JOINROOM:
		ChatJoinChannel(g_enterInfo);
		break;
	case CA_ROOMLIST:
		theApp.OnChatroomList();
		break;
	}
}


void CIrcSocket::ProcessMessage(char *szLine) {
	IRCPARSE	parse;
	CIrcPrint	ircPrint;
	CString		strLine;

#ifdef IRCLOG    // creates IRC message log **ONLY FOR LOCAL DEBUGGING**
	if (!theApp.m_fileIn) {
		static FILE *fp = NULL;
		if (!fp)
			fp = fopen("irc.txt", "w");
		if (fp) {
			fprintf(fp, "%s\n", szLine);
			fflush(fp);
		}
	}
#endif IRCLOG

	ParseIt(szLine, &parse);
	if (parse.nArgs <= 0) {
		ASSERT(0);
		ParseIt(szLine, &parse);
	}
	if (parse.nArgs <= 0) {
		ASSERT(0);		// should never happen?
		return;
	}

	if (0 == parse.uCode)
	{
		// A real command
		HandleCommand(strLine, szLine, &parse, &ircPrint);
	}
	else
	{
		// Result or Error Code
		if (bIsErrorCode(parse.uCode))
			HandleErrorCode(szLine, &parse, &ircPrint);
		else
			HandleResultCode(strLine, szLine, &parse, &ircPrint);
	}

	AddToStatus(ircPrint, szLine);

	FreeParse(&parse);
}


void CIrcSocket::HandleCommand(CString& strLine, char *szLine, PIRCPARSE pParse, CIrcPrint *pIrcPrint)
{
	ASSERT(pParse);
	ASSERT(pIrcPrint);

	SHORT	nCmd = NGetCmd(pParse->args[0]);
	if (-1 == nCmd)
	{
		ASSERT(FALSE);
		// Unknown IRC/X command
		return;
	}

	ASSERT(nCmd < cmdidMax);

	switch (nCmd)
	{
		default:
		{
			#ifdef DEBUG
				TRACE("Unexpected command: sender nick = %s, mach = %s, user = %s, command = %s, last string = %s\n",
				pParse->nick ? pParse->nick : "",
				pParse->machine ? pParse->machine : "",
				pParse->user ? pParse->user : "",
				pParse->args[0] ? pParse->args[0] : "",
				pParse->lastString ? pParse->lastString : "");
				ASSERT(FALSE);
			#endif // DEBUG
			break;
		}

		case cmdidReply:
		case cmdidRequest:
		{
			#ifdef DEBUG
				TRACE("Untreated command: sender nick = %s, mach = %s, user = %s, command = %s, last string = %s\n",
				pParse->nick ? pParse->nick : "",
				pParse->machine ? pParse->machine : "",
				pParse->user ? pParse->user : "",
				pParse->args[0] ? pParse->args[0] : "",
				pParse->lastString ? pParse->lastString : "");
			#endif // DEBUG
			break;
		}

		case cmdidAuth:
		{
			// Legacy IRCX SSPI challenges are intentionally unsupported. Modern
			// authentication is handled by SASL before this parser sees a line.
			pIrcPrint->SetFormat(PT_NONE);
			break;
		}
		case cmdidClone:
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 1, TRUE);
			break;
		}

		case cmdidCreate:
		{
			if (pParse->nArgs >= 3)
			{
				if (bProcessAddChannel(pParse->args[1], NewDefaultProto(NULL), &g_nCXKeepServer, &g_bCXPrompt))
				{
					ASSERT(currentRoom);
					strCurrentChannelTopic = "";
					dwCurrentChannelMode = 0;
					dwCurrentUserLimit = 0;

					CCQuery* pQuery = new CCQuery(qpInitialNames, ctNames, dtMax, NULL, strCurrentChannel, "", FALSE /*bCreatePrUserMatch*/);

					if (pQuery)
						m_queries.bAddQuery(pQuery);

					pQuery = new CCQuery(qpInitialTopic, ctTopic, dtMax, NULL, strCurrentChannel, "", FALSE /*bCreatePrUserMatch*/);

					if (pQuery)
						m_queries.bAddQuery(pQuery);

					ASSERT(GetIrcProto());
					VERIFY(GetIrcProto()->bExecuteQuery(qpInitialMode, ctGetChannelMode, dtMax, NULL, pParse->args[1], ""));
					VERIFY(GetIrcProto()->bExecuteQuery(qpInitialWho, ctWho, dtMax, NULL, pParse->args[1], ""));

					if (m_bIrcXServer)
						VERIFY(GetIrcProto()->bExecuteQuery(qpJoinBackUrl, ctPropGet, dtMax, NULL, pParse->args[1], ""));
				}
				pIrcPrint->SetFormat(PT_NONE);
			}
			break;
		}

		case cmdidData:
		{
			TRACE("Got a Data message! (snick = %s, mach = %s, user = %s)\n", pParse->nick, pParse->machine, pParse->user);

			pIrcPrint->SetFormat(PT_NONE);
			// CCUDI1 = Comic Chat User Display Info version 1
			if (pParse->lastString &&
				pParse->lastString[0] == '#' &&
				pParse->nArgs >= 3 &&
				!strcmp(pParse->args[2], CCUDI1) &&
				*pParse->nick &&
				*pParse->user)
			{
				CChatDoc*	doc = NULL;
				BYTE		msgType;

				if (CHANNELPREFIX(pParse->args[1][0]))
				{
					doc = LookupDoc(pParse->args[1]);
					msgType = MT_CHANNELSEND | MT_DATA;
				}
				else
					msgType = MT_PRIVATEMSG | MT_DATA;

				if ((msgType & MT_PRIVATEMSG) || doc)
				{
					CString strID;

					if (*pParse->machine)
						strID.Format("%s@%s", pParse->user, pParse->machine);

					// interpret "# Appears as" message as being sent to all rooms they are a member of
					if (!doc && strncmp(((LPCTSTR)pParse->lastString)+1, APPEARSPREFIX, g_nAppearsAsLen) == 0)
					{
						POSITION pos = g_docs.GetHeadPosition();
						while (pos)
						{
							doc = (CChatDoc*) g_docs.GetNext(pos);
							CUserInfo *pui = LookupPui(pParse->nick, doc);
							if (pui && !pui->IsDeparted())
								OnDataMsg(doc, pParse->nick, strID, pParse->lastString, msgType);
						}
					}
					else
						OnDataMsg(doc, pParse->nick, strID, pParse->lastString, msgType);
				}
			}
			break;
		}

		case cmdidError:
		{
			if (pParse->lastString) {
				CSInString(&pParse->lastString);
				if (m_bJustSentModeIsIrcX) {
					pIrcPrint->SetFormat(PT_NONE);
					HrModeIsIrcXFailure();
				} else {
					if (strstr(pParse->lastString, "No IRC clients"))
						AfxMessageBox(IDS_MICONLY);
					else
						AfxMessageBox(pParse->lastString);	// print the message verbatim (localization problem!!!)
					AfxGetMainWnd()->PostMessage(WM_COMMAND, ID_FILE_NEW, 0);
				}
			}
			break;
		}

		case cmdidInvite:
		{
			if (pParse->lastString) {
				CSInPlace(pParse->user, sizeof(pParse->user));  // necessary ?
				CString ident(pParse->user);
				ident += "@";
				ident += pParse->machine;
				OnInvite(pParse->nick, ident, pParse->lastString);
				pIrcPrint->SetFormat(PT_NONE);	// handled via popup
			}
			break;
		}

		case cmdidJoin:
		{
			TRACE("Got a JOIN!\n");
			// Modern IRC servers (RFC 2812, e.g. Libera) send "JOIN #channel"
			// with the channel as an ordinary argument rather than a ":"-prefixed
			// trailing parameter, so the parser leaves lastString NULL and puts the
			// channel in args[1].  Recover it so the rest of the handler works.
			if (!pParse->lastString && pParse->nArgs >= 2 && pParse->args[1])
				pParse->lastString = strdup(pParse->args[1]);
			ASSERT(pParse->lastString);

			CSInPlace(pParse->user, sizeof(pParse->user));
			CString strIdent(pParse->user);
			strIdent += "@";
			strIdent += pParse->machine;

			if (stricmp(pParse->nick, GetMyNickName()))
			{				// Don't send or register self
				enumActions	rgaIDs[2] = { (enumActions) 1, aHighlightMessage };
				CDocument*	pDoc = LookupDoc(pParse->lastString);
				if (pDoc)	// else we just left this channel
				{
					theApp.m_dynaRules.bMatchAndApplyRules(eOnJoin, (enumActions*) rgaIDs, NULL, CString(GetMyServer()), CString(pParse->nick)+"!"+strIdent, CString(pParse->lastString), CString(""));
					char cHighlightType = -1;
					if (theApp.m_dynaRules.GetFlags() & g_wHighlight)
						cHighlightType = theApp.m_dynaRules.GetFlags() >> 8;
					AddAndExecute(new JoinEntry(new CUserInfo(pParse->nick, strIdent), FALSE, cHighlightType), pDoc);
					theApp.m_dynaRules.bMatchAndApplyRules(eOnJoin, NULL, (enumActions*) rgaIDs, CString(GetMyServer()), CString(pParse->nick)+"!"+strIdent, CString(pParse->lastString), CString(""));
				}
			}
			else
			{
				// REGISB 11/18/97 added if statement for Fix 4449
				if (bProcessAddChannel(pParse->lastString, NewDefaultProto(NULL), &g_nCXKeepServer, &g_bCXPrompt))
				{
					ASSERT(currentRoom);
					strCurrentChannelTopic = "";
					dwCurrentChannelMode = 0;
					dwCurrentUserLimit = 0;

					// REGISB added 11/07/97
					if (!theApp.m_nMyIdentLength)
					{
						theApp.m_nMyIdentLength = strIdent.GetLength() + 2; // + 2 for the ! and ~ signs
						SetMyIdent(strIdent);
					}

					CCQuery* pQuery = new CCQuery(qpInitialNames, ctNames, dtMax, NULL, strCurrentChannel, "", FALSE /*bCreatePrUserMatch*/);

					if (pQuery)
						m_queries.bAddQuery(pQuery);

					pQuery = new CCQuery(qpInitialTopic, ctTopic, dtMax, NULL, strCurrentChannel, "", FALSE /*bCreatePrUserMatch*/);

					if (pQuery)
						m_queries.bAddQuery(pQuery);

					ASSERT(GetIrcProto());
					VERIFY(GetIrcProto()->bExecuteQuery(qpInitialMode, ctGetChannelMode, dtMax, NULL, pParse->lastString, ""));
					VERIFY(GetIrcProto()->bExecuteQuery(qpInitialWho, ctWho, dtMax, NULL, pParse->lastString, ""));

					if (m_bIrcXServer)
						VERIFY(GetIrcProto()->bExecuteQuery(qpJoinBackUrl, ctPropGet, dtMax, NULL, pParse->lastString, ""));
				}
			}
			pIrcPrint->SetFormat(PT_NONE);
			break;
		}

		case cmdidKick:
		{
			if (pParse->lastString && pParse->nArgs >= 3)
			{
				CChatDoc*	pDoc = LookupDoc(pParse->args[1]);
				CSInString(&pParse->lastString, pParse->args[1], pDoc);
				OnKick(pDoc, pParse->nick, pParse->args[2], pParse->lastString);
			}
			pIrcPrint->SetFormat(PT_NONE);		   // kicks only appear in channel window
			break;
		}

		case cmdidKnock:
		{
			pIrcPrint->SetFormat(PT_WHOLESTRING, szLine, RGB(0,0,0), 0, TRUE);
			break;
		}

		case cmdidMode:
		{
			if (pParse->nArgs >= 3)	// Channel mode change
			{
				const char *szFlags, *szArg2 = "", *szArg3 = "";

				szFlags = pParse->args[2];
				if (pParse->nArgs >= 4)
					szArg2 = pParse->args[3];
				if (pParse->nArgs >= 5)
					szArg3 = pParse->args[4];

				CChatDoc *doc = LookupDoc(pParse->args[1]);
				if (doc)
					ParseChannelMode(doc, szFlags, szArg2, szArg3, NULL);

				// Get oldest queued ctSetChannelMode query object
				POSITION		pos;
				CCQuery*		pQuery = m_queries.FindQuery(ctSetChannelMode, &pos);
				char*			szTmp;
				static MODECACH	mcLost = { NULL, NULL, MC_NONE };

				if (pQuery)
				{
					switch (pQuery->GetQueryPurpose())
					{
					case qpComSetChannelMode:
						ASSERT(pos);
						ASSERT(0 == stricmp(pQuery->GetChannelName(), pParse->args[1]));
						m_queries.FreeRemoveAt(pos);
						pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 1, TRUE);
						if (NULL != (szTmp = (char *)strstr(szFlags, "-o")) || NULL != (char *)strstr(szFlags, "-q"))
						{
							ZeroMemory((PVOID) &mcLost, sizeof(MODECACH));
							strncpy(mcLost.szChannelName, pParse->args[1], MAX_TOKEN-1);
							strncpy(mcLost.szNickname, szArg2, MAX_NICK-1);
							mcLost.byteStatus = szTmp ? MC_HOSTLOST : MC_OWNERLOST;
						}
						else
							mcLost.byteStatus = MC_NONE;
						break;
					default:
						ASSERT(FALSE);
					}
				}
				else
				{
					if (MC_NONE != mcLost.byteStatus &&
						!strcmp(mcLost.szChannelName, pParse->args[1]) &&
						!strcmp(mcLost.szNickname, szArg2))
					{
						if ((MC_HOSTLOST == mcLost.byteStatus && strstr(szFlags, "+q")) ||
							(MC_OWNERLOST == mcLost.byteStatus && strstr(szFlags, "+o")) ||
							(MC_OWNERLOST == mcLost.byteStatus && strstr(szFlags, "-o")))
							pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 1, TRUE);
						else
							pIrcPrint->SetFormat(PT_NONE);

						mcLost.byteStatus = MC_NONE;
					}
					else
						pIrcPrint->SetFormat(PT_NONE);
				}
			}
			else
				if (pParse->nArgs == 2)	// User mode change
				{
					// Our visibility could change for example
					if (0 == stricmp(pParse->args[1], GetMyNickName()))
					{
						// User mode change is for us

						// Get oldest queued ctSetUserMode query object
						POSITION	pos;
						CCQuery*	pQuery = m_queries.FindQuery(ctSetUserMode, &pos);
						BOOL		bDisplay = TRUE;

						if (pQuery)
						{
							ASSERT(pos);
							switch (pQuery->GetQueryPurpose())
							{
								case qpSetInvisible:
								case qpSetVisible:
								case qpComSetUserMode:
								{
									// REGISB this code should be turned into a generic function like ApplyIRCToOurUserMode
									// of MsChatPr if we decide to treat all user modes: a i s w o
									ASSERT(0 == pQuery->GetNicknameMask().CompareNoCase(GetMyNickName()));
									BOOL	bSet, bInvisibility = FALSE, bRemoveCell = (qpComSetUserMode == pQuery->GetQueryPurpose());
									LPTSTR	szModes = pParse->lastString;
									ASSERT(szModes);
									while (*szModes)
									{
										switch (*szModes)
										{
											case '-':
												bSet = FALSE;
												break;
											case '+':
												bSet = TRUE;
												break;
											case 'i':
												theApp.m_flags1 = bSet ? (theApp.m_flags1 & ~F1_USERVISIBLE) : (theApp.m_flags1 | F1_USERVISIBLE);
												if (qpComSetUserMode != pQuery->GetQueryPurpose())
													bDisplay = FALSE;
												bRemoveCell = TRUE;
												break;
										}
										szModes++;
									}
									if (bRemoveCell)
										m_queries.FreeRemoveAt(pos);
									break;
								}
								default:
									ASSERT(FALSE);
							}
						}
						if (bDisplay)
							pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 1, TRUE);
						else
							pIrcPrint->SetFormat(PT_NONE);
					}
				}
			break;
		}

		case cmdidNick:
		{
			pIrcPrint->SetFormat(PT_NONE);
			if (pParse->lastString)
			{
				BOOL		bSetName = FALSE, bDisplayNewNickInStatusWnd;
				POSITION	pos;

				// Is our own nickname changing?
				bDisplayNewNickInStatusWnd = (0 == strcmp(pParse->nick, GetMyNickName()));

				pos = g_docs.GetHeadPosition();
				while (pos)
				{
					CChatDoc *doc = (CChatDoc*) g_docs.GetNext(pos);
					if (doc->m_proto->GetConnectionStatus() == CX_INCHANNEL)
					{
						CUserInfo *pui = LookupPui(pParse->nick, doc);
						if (pui && !pui->IsDeparted())
						{
							AddAndExecute(new NickEntry(pParse->nick, pParse->lastString), doc);
							bSetName = TRUE;
						}
					}
				}

				if (!bSetName)
					SetMyNameNick(pParse->lastString);

				if (bDisplayNewNickInStatusWnd)
				{
					strLine.Format(IDS_NOWKNOWNAS, GetMyScreenName());
					pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,0,255), 0, TRUE);
				}
			}
			break;
		}

		case cmdidNotice:
		case cmdidPrivMsg:
		{
			pIrcPrint->SetFormat(PT_NONE);    // don't print these in status window

			TRACE("Got a PrivMsg! (snick = %s, mach = %s, user = %s)\n", pParse->nick, pParse->machine, pParse->user);

			if (pParse->lastString && pParse->nArgs >= 2)
				if (*pParse->nick && *pParse->user)
				{
					CChatDoc	*doc = NULL;
					BYTE		msgType;

					msgType = (cmdidPrivMsg == nCmd) ? MT_PRVMSG : MT_NOTICE;

					if (CHANNELPREFIX(pParse->args[1][0]))
					{
						doc = LookupDoc(pParse->args[1]);
						msgType |= MT_CHANNELSEND;
					}
					else
						msgType |= MT_PRIVATEMSG;

					if ((msgType & MT_PRIVATEMSG) || doc)
					{
						CString strID;
						if (*pParse->user && *pParse->machine)
							strID.Format("%s@%s", pParse->user, pParse->machine);

						// interpret "# Appears as" message as being sent to all rooms they are a member of
						if (!doc && pParse->lastString[0] == '#' && strncmp(((LPCTSTR)pParse->lastString)+1, APPEARSPREFIX, g_nAppearsAsLen) == 0)
						{
							POSITION pos = g_docs.GetHeadPosition();
							while (pos)
							{
								doc = (CChatDoc *)g_docs.GetNext(pos);
								CUserInfo *pui = LookupPui(pParse->nick, doc);
								if (pui && !pui->IsDeparted())
									OnTextMsg(doc, pParse->nick, strID, pParse->lastString, msgType);
							}
						}
						else
						{
							CSInString(&pParse->lastString, pParse->args[1], doc);
							OnTextMsg(doc, pParse->nick, strID, pParse->lastString, msgType);
						}
					}
				}
				else
					if (!*pParse->nick && !*pParse->user)
						pIrcPrint->SetFormat(PT_LASTSTRING, szLine, RGB(128,0,128));
			break;
		}

		case cmdidPart:
		{
			TRACE("Got a PART!\n");
			CChatDoc *pDoc = LookupDoc(pParse->args[1]);
			if ((stricmp(pParse->nick, GetMyNickName()) == 0))
			{
				GotPartChannel(pDoc);
				theApp.m_pExitingDoc = NULL;
			}
			else
				if (pDoc)
				{
					enumActions	rgaIDs[2] = { (enumActions) 1, aHighlightMessage };
					CString		strIdent = CString(pParse->nick)+"!"+pParse->user+"@"+pParse->machine;

					theApp.m_dynaRules.bMatchAndApplyRules(eOnLeave, (enumActions*) rgaIDs, NULL, CString(GetMyServer()), strIdent, CString(pParse->args[1]), CString(""));
					char cHighlightType = -1;
					if (theApp.m_dynaRules.GetFlags() & g_wHighlight)
						cHighlightType = theApp.m_dynaRules.GetFlags() >> 8;
					AddAndExecute(new PartEntry(pParse->nick, cHighlightType), pDoc);
					theApp.m_dynaRules.bMatchAndApplyRules(eOnLeave, NULL, (enumActions*) rgaIDs, CString(GetMyServer()), strIdent, CString(pParse->args[1]), CString(""));
				}
			pIrcPrint->SetFormat(PT_NONE);
			break;
		}

		case cmdidPing:
		{
			TryFormatOutBuff( "PONG :%s\r\n", pParse->lastString ? pParse->lastString : "");
			TRACE("%s", GetOutBuff());
			Send(GetOutBuff(), strlen(GetOutBuff()));
			pIrcPrint->SetFormat(PT_NONE);
			break;
		}

		case cmdidPong:
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 1, TRUE);
			break;
		}

		case cmdidProp:
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 2);		// may be overridden
			if (pParse->nArgs == 3 && pParse->lastString)
			{
				if (CHANNELPREFIX(pParse->args[1][0]))
				{
					CChatDoc* doc = LookupDoc(pParse->args[1]);
					// Also verify that it's an IRCX channel
					if (doc != NULL && doc->m_proto->IsIRCX ())
					{
						// Look for the CLIENT property being set, and handle it.
						if (!lstrcmp (pParse->args[2], "CLIENT"))
						{
							// Get oldest queued ctPropSet query object
							POSITION	pos;
							CCQuery*	pQuery = m_queries.FindQuery(ctPropSet, &pos);

							if (pQuery && pQuery->GetQueryPurpose() == qpSetClient)
							{
								ASSERT(pos);
								ASSERT(0 == stricmp(pQuery->GetChannelName(), pParse->args[1]));
								m_queries.FreeRemoveAt(pos);
								pIrcPrint->SetFormat(PT_NONE);
							}

							((CIrcProto *)doc->m_proto)->HandleClientDataChange (pParse->lastString);
						}
						if (!lstrcmp (pParse->args[2], "TOPIC"))
						{
							// Channel topic changed via PROP command
							CSInString(&pParse->lastString, pParse->args[1], doc);
							if (doc->m_proto->m_prgdwTopicFormatting)
								doc->m_proto->m_prgdwTopicFormatting->RemoveAll();
							else
								doc->m_proto->m_prgdwTopicFormatting = new CDWordArray;
							doc->m_proto->m_strTopic = SzControlLess(pParse->lastString, doc->m_proto->m_prgdwTopicFormatting);
						}
					}
				}
			}
			break;
		}

		case cmdidKilled:
		{
			// user successfully killed another one
			pIrcPrint->SetFormat(PT_WHOLESTRING, szLine, RGB(0,0,255), 0, TRUE);
			break;
		}

		case cmdidKill:
		case cmdidQuit:		// collapse w/ PART?
		{
			LPCTSTR		szQuittingNick = (nCmd == cmdidQuit) ? pParse->nick : pParse->args[1];
			POSITION	pos = g_docs.GetHeadPosition();

			while (pos)
			{
				CChatDoc *pDoc = (CChatDoc*) g_docs.GetNext(pos);
				if (pDoc->GetConnectionStatus() != CX_INCHANNEL)
					break;
				CUserInfo *pui = LookupPui(szQuittingNick, pDoc);
				if (pui && !pui->IsDeparted())
					AddAndExecute(new PartEntry(szQuittingNick), pDoc);
			}
			pIrcPrint->SetFormat(PT_NONE);	// let's not display QUIT or KILL messages in the Status Window
			break;
		}

		case cmdidTopic:
		{
			if (pParse->nArgs >= 2 && pParse->lastString)
			{
				CString		strCtrlLessTopic;
				CDWordArray	rgdwFormattingTmp;
				CChatDoc*	pDoc = LookupDoc(pParse->args[1]);

				CSInString(&pParse->lastString, pParse->args[1], pDoc);

				strCtrlLessTopic = SzControlLess(pParse->lastString, &rgdwFormattingTmp);

				if (pDoc)
				{
					if (pDoc->m_proto->m_prgdwTopicFormatting)
						FreeAndNullFormatting(&pDoc->m_proto->m_prgdwTopicFormatting);

					pDoc->m_proto->m_prgdwTopicFormatting = CopyFormatting(&rgdwFormattingTmp);
					pDoc->m_proto->m_strTopic = strCtrlLessTopic;
				}

				// Get oldest queued ctTopic query object
				POSITION	pos;
				CCQuery*	pQuery = m_queries.FindQuery(ctTopic, &pos);

				if (pQuery)
				{
					ASSERT(pos);
					switch (pQuery->GetQueryPurpose())
					{
						case qpSetTopic:
							ASSERT(0 == strcmp(pParse->args[1], pQuery->GetChannelName()));
							break;
						default:
							ASSERT(FALSE);
					}
					m_queries.FreeRemoveAt(pos);
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
				{
					strLine = CString(pParse->args[1]) + " :" + strCtrlLessTopic;
					PushFormattingOffsets(&rgdwFormattingTmp, strlen(pParse->args[1]) + 2);
					pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,0,128), 0, TRUE);
					AddToStatus(*pIrcPrint, strLine, &rgdwFormattingTmp);
					pIrcPrint->SetFormat(PT_NONE);			// Don't display this a second time
				}

				rgdwFormattingTmp.RemoveAll();
			}
			break;
		}

		case cmdidWhisper:
		{
			CChatDoc *doc = LookupDoc(pParse->args[1]);
			if (doc && *pParse->nick) {
				CDWordArray talkTos;
				// compute talktos array, just in case message isn't cooked...
				void GetTalkTos(CChatDoc *doc, CDWordArray *talkTos, char *str);
				GetTalkTos(doc, &talkTos, pParse->args[2]);
				CSInString(&pParse->lastString, pParse->args[1], doc);
				OnTextMsg(doc, pParse->nick, "X", pParse->lastString, MT_PRIVATEMSG | MT_WHISPER, &talkTos); // for now, treated similarly to Private Message
			}
			break;
		}
	}
}


void CIrcSocket::HandleResultCode(CString &strLine, char *szLine, PIRCPARSE pParse, CIrcPrint *pIrcPrint)
{
	static CRoom	*sRoom;
	static BOOL		sbAddIt;

	ASSERT(pParse);
	ASSERT(pIrcPrint);
	ASSERT(pParse->uCode);

	switch (pParse->uCode)
	{
		default:
		{
			#ifdef DEBUG
				TRACE("Untreated reply: sender nick = %s, mach = %s, user = %s, reply = %s, last string = %s\n",
				pParse->nick ? pParse->nick : "",
				pParse->machine ? pParse->machine : "",
				pParse->user ? pParse->user : "",
				pParse->args[0] ? pParse->args[0] : "",
				pParse->lastString ? pParse->lastString : "");
			#endif // DEBUG
			break;
		}

		case RPL_WELCOME:		// 001
		{
			// Woohoo! We're in!
			theApp.CompleteConnection ();

			if (pParse->nArgs >= 2)				// We're logged in!  Join channel or show room list, or do nothing
				SetMyNameNick(pParse->args[1]);	// It's what the server thinks..
			pIrcPrint->SetFormat(PT_LASTSTRING, szLine, RGB(255,0,0));
			AddToStatus(*pIrcPrint, szLine);		// We need to do this before OnLogin displays the RoomList dialog box
			pIrcPrint->SetFormat(PT_NONE);		// Don't display this a second time

			GetIrcProto()->SetConnectionStatus(CX_NOCHANNEL);
			theApp.m_dynaRules.bMatchAndApplyRules(eOnConnect, NULL, NULL, CString(GetMyServer()), CString(pParse->args[1]), CString(""), CString(""));
			if (GetIrcProto()->GetConnectionStatus() != CX_DISCONNECTED)	// rules might have disconnected us
			{
				CCQuery* pQuery = new CCQuery(qpInitialLUsersMOTD, ctLUsersMOTD, dtMax, NULL, "", "", FALSE /*bCreatePrUserMatch*/);
				if (pQuery)
					m_queries.bAddQuery(pQuery);

				GetIrcProto()->OnLogin();
			}
			break;
		}

		case RPL_YOURHOST:		// 002
		case RPL_CREATED:		// 003
		{
			pIrcPrint->SetFormat(PT_LASTSTRING, szLine, RGB(255,0,0));
			break;
		}

		case RPL_MYINFO:		// 004
		case RPL_FOOFORNOW:		// 005
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(255,0,0), 3, TRUE);
			break;
		}

		case RPL_TRACELINK:		// 200
		case RPL_TRACECONNECTING://201
		case RPL_TRACEHANDSHAKE:// 202
		case RPL_TRACEUNKNOWN:	// 203
		case RPL_TRACEOPERATOR:	// 204
		case RPL_TRACEUSER:		// 205
		case RPL_TRACESERVER:	// 206
		case RPL_TRACENEWTYPE:	// 208
		case RPL_TRACELOG:		// 261

		case RPL_STATSLINKINFO:	// 211
		case RPL_STATSCOMMANDS:	// 212
		case RPL_STATSCLINE:	// 213
		case RPL_STATSNLINE:	// 214
		case RPL_STATSILINE:	// 215
		case RPL_STATSKLINE:	// 216
		case RPL_STATSYLINE:	// 218
		case RPL_ENDOFSTATS:	// 219
		case RPL_STATSLLINE:	// 241
		case RPL_STATSUPTIME:	// 242
		case RPL_STATSOLINE:	// 243
		case RPL_STATSHLINE:	// 244

		case RPL_ADMINME:		// 256
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 3, RPL_ENDOFSTATS == pParse->uCode);
			break;
		}

		case RPL_ADMINLOC1:		// 257
		case RPL_ADMINLOC2:		// 258
		case RPL_ADMINEMAIL:	// 259
		{
			pIrcPrint->SetFormat(PT_LASTSTRING, szLine, RGB(0,0,0), 0, FALSE);
			break;
		}

		case RPL_UMODEIS:		// 221
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 2, TRUE);
			break;

		case RPL_LUSERCLIENT:	// 251
		case RPL_LUSEROP:		// 252
		case RPL_LUSERUNKNOWN:	// 253
		case RPL_LUSERCHANNELS:	// 254
		case RPL_LUSERME:		// 255
		case RPL_LOCALUSERS:	// 265
		case RPL_GLOBALUSERS:	// 266
		{
			CSInString(&pParse->lastString);
			const char *szLUser = pParse->lastString;
			if (szLUser)
			{
				if (pParse->nArgs >= 3)
				{	// add first arg as string prefix...  Necessary for commands 252-254
					strLine = pParse->args[2];
					strLine += " ";
				}
				strLine += szLUser;

				CCQuery* pQuery = m_queries.FindQuery(ctLUsersMOTD, NULL);

				if (pQuery)
				{
					m_strLUSER += strLine;
					m_strLUSER += "\n";
				}

				if (pQuery && pQuery->GetQueryPurpose() == qpLUsersMOTD)
					pIrcPrint->SetFormat(PT_NONE);
				else
					pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,0,255), 0, RPL_GLOBALUSERS == pParse->uCode);
			}
			break;
		}

		case RPL_USERHOST:		// 302
		{
			strLine.Format(IDS_USERHOST_PREFIX, pParse->lastString);
			pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(128,0,128), 0, TRUE);
			break;
		}

		case RPL_ISON:			// 303
		{
			strLine.Format(IDS_ISON_PREFIX, pParse->lastString);
			pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,0,255), 0, TRUE);
			break;
		}

		case RPL_AWAY:			// 301
		{
			if (pParse->lastString && pParse->nArgs > 2)
			{
				strLine.LoadString(IDS_AWAYREPORT);
				VERIFY(ReplaceToken(strLine, CString("%1"), DecodeNickForScreen(pParse->args[2])));
				VERIFY(ReplaceToken(strLine, CString("%2"), pParse->lastString));

				pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,128,128), 0, TRUE);
			}
			break;
		}

		case RPL_UNAWAY:		// 305
		case RPL_NOWAWAY:		// 306
		{
			pIrcPrint->SetFormat(PT_LASTSTRING, szLine, RGB(0,128,128), 0, TRUE);
			break;
		}

		case RPL_WHOISUSER:		// 311
		{
			if (pParse->nArgs >= 5)
			{
				CSInString(&pParse->args[3]);	// user name

				// Get oldest queued ctWhoIs query object
				CCQuery*	pQuery = m_queries.FindQuery(ctWhoIs, NULL);

				// REGISB: pQuery can be NULL in user sends /RAW WHOIS KingArthur
				if (pQuery)
				{
					ASSERT(0 == stricmp(pQuery->GetNicknameMask(), pParse->args[2]));
					switch (pQuery->GetQueryPurpose())
					{
						case qpBanDlg:
						case qpKickDlg:
						{
							CString strBan;
							GetBanString(pParse->args[3], pParse->args[4], strBan);

							ASSERT(currentRoom);
							if (qpKickDlg == pQuery->GetQueryPurpose())
								currentRoom->DoKickDlg(pParse->args[2], strBan);
							else
							{
								g_strBan = strBan;
								TryFormatOutBuff( "MODE %s +b\r\n", pQuery->GetChannelName());
								currentRoom->SendMessageText(GetOutBuff());
							}
							break;
						}
						case qpGetIdent:
						{
							ShowIdentity(pParse->args[2], pParse->args[3], pParse->args[4]);
							break;
						}
						case qpIgnoreIdent:
						{
							CString	strFullName(pParse->args[3]);
							strFullName += "@";
							strFullName += pParse->args[4];
							IgnoreUser(pParse->args[2], strFullName, ((WORD) pQuery->GetData()) & g_wIgnoreIdent, ((WORD) pQuery->GetData()) & g_wAutoIgnoreIdent);
							break;
						}
						default:
							ASSERT(FALSE);
					}
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
					pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,128), 3);
			}
			break;
		}

		case RPL_WHOISSERVER:	// 312
		case RPL_WHOISOPERATOR:	// 313
		case RPL_WHOISIDLE:		// 317
		case RPL_WHOISCHANNELS:	// 319
		case RPL_WHOISIP:		// 320
		{
			// Get oldest queued ctWhoIs query object
			if (m_queries.FindQuery(ctWhoIs, NULL))
				pIrcPrint->SetFormat(PT_NONE);
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,128), 3);
			break;
		}

		case RPL_ENDOFWHOIS:	// 318
		{
			if (pParse->nArgs >= 3)
			{
				// Get oldest queued ctWhoIs query object
				POSITION	pos;
				CCQuery*	pQuery = m_queries.FindQuery(ctWhoIs, &pos);

				// REGISB: pQuery can be NULL in user sends /RAW WHOIS KingArthur
				if (pQuery)
				{
					ASSERT(pos);
					ASSERT(0 == stricmp(pQuery->GetNicknameMask(), pParse->args[2]));
					m_queries.FreeRemoveAt(pos);
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
					pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,128), 3, TRUE);
			}
			break;
		}

		case RPL_WHOWASUSER:	// 314
		case RPL_ENDOFWHOWAS:	// 369
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,128), 3, pParse->uCode == RPL_ENDOFWHOWAS);
			break;
		}

		case RPL_LINKS:			// 364
		case RPL_ENDOFLINKS:	// 365
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 3, pParse->uCode == RPL_ENDOFLINKS);
			break;
		}

		case RPL_CHANNELMODEIS:	// 324
		{
			if (pParse->nArgs >= 4)
			{
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 3);		// may be overridden

				CChatDoc *doc = LookupDoc(pParse->args[2]);
				if (doc)
				{
					CRoomInfo	*pEnterInfo;
					INT			iRoomInfo;
					const char	*szArg2 = "", *szArg3 = "";
					if (pParse->nArgs >= 5)
						szArg2 = pParse->args[4];
					if (pParse->nArgs >= 6)
						szArg3 = pParse->args[5];
					ASSERT(doc->m_proto);
					doc->m_proto->m_strPassword = "";
					doc->m_proto->m_dwModes = 0;		// next ParseChannelMode is absolute, not relative
					pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) pParse->args[2], &iRoomInfo, NULL != (char *)strchr(pParse->args[3], 'e') /*bCloneOK*/);
					ParseChannelMode(doc, pParse->args[3], szArg2, szArg3, pEnterInfo);

					if (currentRoom && pEnterInfo->m_bSetMode && !stricmp(pParse->args[2], currentRoom->m_strChannel))
					{	// set modes if requested on channel creation
						ASSERT(pEnterInfo == &g_enterInfo);
						doc->m_proto->ChatSetMode(pEnterInfo->m_dwModes, pEnterInfo->m_dwMaxUsers, pEnterInfo->m_strPassword);
						if (!pEnterInfo->m_strTopic.IsEmpty())
						{
							CString strControlFull = pEnterInfo->m_strTopic;
							if (pEnterInfo->m_prgdwTopicFormatting)
							{
								char* szCtrlFull = SzControlFull((LPCTSTR) pEnterInfo->m_strTopic, pEnterInfo->m_prgdwTopicFormatting);
								if (szCtrlFull)
								{
									strControlFull = CString(szCtrlFull);
									delete [] szCtrlFull;
								}
							}
							doc->m_proto->ChatSetTopic(strControlFull);
						}
					}

					if (iRoomInfo > 0)
						theApp.RemoveRoomInfo(iRoomInfo);
					else
						bInitEnterInfo(*pEnterInfo, "", NULL, NULL, 0L, FALSE);		// we've already taken care of the connecting sequence
				}

				// Get oldest queued ctGetChannelMode query object
				POSITION	pos;
				CCQuery*	pQuery = m_queries.FindQuery(ctGetChannelMode, &pos);

				if (pQuery)
				{
					ASSERT(pos);
					switch (pQuery->GetQueryPurpose())
					{
						case qpInitialMode:
							ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
							break;
						default:
							ASSERT(FALSE);
					}
					m_queries.FreeRemoveAt(pos);
					pIrcPrint->SetFormat(PT_NONE);
				}
			}
			break;
		}

		case RPL_NOTOPIC:		// 331
		{
			CCQuery*	pQuery;
			POSITION	pos;

			if (pQuery = m_queries.FindQuery(ctTopic, &pos))
			{
				ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
				ASSERT(qpListMembers == pQuery->GetQueryPurpose());
				pIrcPrint->SetFormat(PT_NONE);
				CString strEncodedChannel = pQuery->GetChannelName();
				CString strPrettyChannel  = pQuery->GetData() ? (LPTSTR) pQuery->GetData() : "";
				ASSERT(pos);
				m_queries.FreeRemoveAt(pos);
				OnUserListAux(NULL, strEncodedChannel, strPrettyChannel);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,128), 3, TRUE);
			break;
		}

		case RPL_TOPIC:			// 332
		{
			if (pParse->nArgs >= 3 && pParse->lastString)
			{
				CString		strEncodedChannel;
				CString		strPrettyChannel;
				CString		strCtrlLessTopic;
				CDWordArray	rgdwFormattingTmp;
				CChatDoc*	pDoc = LookupDoc(pParse->args[2]);
				CCQuery*	pQuery = NULL;
				POSITION	pos = NULL;
				BOOL		bListMembers = FALSE;

				CSInString(&pParse->lastString, pParse->args[2], pDoc);

				strCtrlLessTopic = SzControlLess(pParse->lastString, &rgdwFormattingTmp);

				if (pDoc)
				{
					if (pDoc->m_proto->m_prgdwTopicFormatting)
						FreeAndNullFormatting(&pDoc->m_proto->m_prgdwTopicFormatting);

					pDoc->m_proto->m_prgdwTopicFormatting = CopyFormatting(&rgdwFormattingTmp);
					pDoc->m_proto->m_strTopic = strCtrlLessTopic;

					// Get oldest queued ctTopic query object
					pQuery = m_queries.FindQuery(ctTopic, &pos);
				}

				if (pDoc && pQuery)
				{
					ASSERT(pos);
					switch (pQuery->GetQueryPurpose())
					{
						case qpInitialTopic:
							ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
							break;
						case qpListMembers:
							ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
							bListMembers = TRUE;
							break;
						default:
							ASSERT(FALSE);
					}
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
				{
					if (pQuery = m_queries.FindQuery(ctTopic, &pos))
					{
						ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
						ASSERT(qpListMembers == pQuery->GetQueryPurpose());
						bListMembers = TRUE;
					}
					else
					{
						strLine = CString(pParse->args[2]) + " :" + strCtrlLessTopic;
						PushFormattingOffsets(&rgdwFormattingTmp, strlen(pParse->args[2]) + 2);
						pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,0,128), 0, TRUE);
						AddToStatus(*pIrcPrint, strLine, &rgdwFormattingTmp);
						// Don't display this a second time
					}
					pIrcPrint->SetFormat(PT_NONE);
				}

				rgdwFormattingTmp.RemoveAll();

				if (pQuery && pos)
				{
					if (bListMembers)
					{
						strEncodedChannel = pQuery->GetChannelName();
						strPrettyChannel  = pQuery->GetData() ? (LPTSTR) pQuery->GetData() : "";
					}
					m_queries.FreeRemoveAt(pos);
				}

				if (bListMembers)
					OnUserListAux(NULL, strEncodedChannel, strPrettyChannel);
			}
			break;
		}

		case RPL_INVITING:		// 341
		{
			if (pParse->nArgs >= 4)
			{
				AcknowledgeInvite(DecodeNick(pParse->args[2]), DecodeChan(pParse->args[3]));  // don't know if it's MIC
				pIrcPrint->SetFormat(PT_NONE);
			}
			break;
		}

		case RPL_LISTSTART:		// 321
		case RPL_LISTXSTART:	// 811
		{
			enumCommandType	ct = (pParse->uCode == RPL_LISTSTART) ? ctList : ctListX;
			CCQuery*		pQuery = m_queries.FindQuery(ct, NULL);

			if (pQuery)
			{
				switch (pQuery->GetQueryPurpose())
				{
					case qpOnNewRoomEvent:
						break;
					case qpRoomListDlg:
					{
						sRoom = NULL;
						g_bCanViewUnrated = bCanViewUnrated();
						StartRoomList();
						break;
					}
					default:
						ASSERT(FALSE);
				}
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,0,128), 3);
			break;
		}

		case RPL_LIST:			// 322
		{
			if (pParse->nArgs >= 4 && pParse->lastString)
			{
				// Get oldest queued ctList query object
				CCQuery*	pQuery = m_queries.FindQuery(ctList, NULL);

				if (pQuery)
				{
					switch (pQuery->GetQueryPurpose())
					{
						case qpOnNewRoomEvent:
						{
							CCRule* pRule = (CCRule*) pQuery->GetData();
							ASSERT(pRule);
							if (pRule->bActive() && !pRule->bStopped())
							{
								CCDaemonExt* pDaemonExt = pRule->GetDaemonExt();
								ASSERT(pDaemonExt);
								CString channelName(pParse->args[2]);
								pDaemonExt->bAddChannelToCurrentList(channelName);
							}
							break;
						}
						case qpRoomListDlg:
						{
							CRoom *pRoom = new CRoom;
							if (pRoom)
							{
								CSInString(&pParse->lastString);
								// Some IRC servers return * as the room name when the
								// room is private.
								if (!pParse->args[2] || pParse->args[2][0] != '*' ||
										pParse->args[2][1] != '\0')
								{
									pRoom->m_name = pParse->args[2];
									pRoom->m_prettyName = DecodeChan(pParse->args[2]);
									pRoom->m_nUsers = atoi(pParse->args[3]);
									pRoom->m_descr = pParse->lastString;
									pRoom->m_byteRegistered = FALSE;
									AddToRoomList(pRoom);  // PICS test done in OnChatRoomList
								}
							}
							break;
						}
						default:
							ASSERT(FALSE);
					}
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
					pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,0,128), 3);
			}
			break;
		}

		case RPL_LISTXLIST:		// 812
		{
			// Get oldest queued ctListX query object
			CCQuery*	pQuery = m_queries.FindQuery(ctListX, NULL);

			if (pQuery)
			{
				switch (pQuery->GetQueryPurpose())
				{
					case qpOnNewRoomEvent:
					{
						CCRule* pRule = (CCRule*) pQuery->GetData();
						ASSERT(pRule);
						if (pRule->bActive() && !pRule->bStopped())
						{
							CCDaemonExt* pDaemonExt = pRule->GetDaemonExt();
							ASSERT(pDaemonExt);
							CString channelName(pParse->args[2]);
							pDaemonExt->bAddChannelToCurrentList(channelName);
						}
						break;
					}
					case qpRoomListDlg:
					{
						if (sRoom)
						{
							AddToRoomList(sRoom, sbAddIt);
							sRoom = NULL;
						}
						if (pParse->nArgs >= 6 && pParse->lastString)
						{
							if (sRoom = new CRoom)
							{
								const char *szRoomName = pParse->args[2];
								BOOL bMIC = (strchr(pParse->args[3], 'y') != NULL);
								CSInString(&pParse->lastString, bMIC ? NULL : szRoomName);
								sRoom->m_name = szRoomName;
								sRoom->m_prettyName = DecodeChan(szRoomName, bMIC);
								sRoom->m_nUsers = atoi(pParse->args[4]);
								sRoom->m_descr = SzControlLess(pParse->lastString, NULL);
								sRoom->m_byteRegistered = (strchr(pParse->args[3], 'r') != NULL);
								sbAddIt = g_bCanViewUnrated;  // default
							}
						}
						break;
					}
					default:
						ASSERT(FALSE);
				}
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,0,128), 3);
			break;
		}

		case RPL_LISTXPICS:		// 813
		{
			sbAddIt = bPassesRatings(pParse->lastString);	// PICS string (server-based.  should already be in windows charset.)

			if (m_queries.FindQuery(ctListX, NULL))
				pIrcPrint->SetFormat(PT_NONE);
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,0,128), 3);
			break;
		}

		case RPL_LISTEND:		// 323
		case RPL_LISTXTRUNC:	// 816
		case RPL_LISTXEND:		// 817
		{
			// Get oldest queued ctList query object
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(pParse->uCode == RPL_LISTEND ? ctList : ctListX, &pos);

			if (pQuery)
			{
				ASSERT(pos);
				m_queries.RemoveAt(pos);
				switch (pQuery->GetQueryPurpose())
				{
					case qpOnNewRoomEvent:
					{
						CCRule*	pRule = (CCRule*) pQuery->GetData();
						ASSERT(pRule);
						if (pRule->bActive() && !pRule->bStopped())
						{
							CCDaemonExt* pDaemonExt = pRule->GetDaemonExt();
							ASSERT(pDaemonExt);
							VERIFY(pDaemonExt->bOnEndOfListing(&(theApp.m_dynaRules), pRule, pQuery->GetQueryPurpose()));
						}
						break;
					}
					case qpRoomListDlg:
					{
						if (sRoom)
						{
							AddToRoomList(sRoom, sbAddIt);
							sRoom = NULL;
						}
						EndRoomList();
						break;
					}
					default:
						ASSERT(FALSE);
				}
				delete pQuery;
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,0,128), 3, TRUE);
			break;
		}

		case RPL_NAMEREPLY:		// 353
		{
			// Get oldest queued ctNames query object
			POSITION	pos;
			CCQuery*	pQuery;

			// We might not have gotten a RPL_TOPIC (332) reply and need to remove the qpInitialTopic cell
			if (pQuery = m_queries.FindQuery(ctTopic, &pos))
			{
				ASSERT(pos);
				switch (pQuery->GetQueryPurpose())
				{
					case qpInitialTopic:
						ASSERT(0 == strcmp(pParse->args[3], pQuery->GetChannelName()));
						break;
					default:
						ASSERT(FALSE);
				}
				m_queries.FreeRemoveAt(pos);
			}

			if (pQuery = m_queries.FindQuery(ctNames, NULL))
			{
				switch (pQuery->GetQueryPurpose())
				{
					case qpInitialNames:
					{
						CChatDoc* pDoc = pParse->nArgs >= 4 ? LookupDoc(pParse->args[3]) : NULL;
						if (pParse->lastString && pDoc)
							if (bForEachWord(pParse->lastString, bSingleJoin, pDoc, 0L, " "))
								theApp.m_dynaRules.bMatchAndApplyRules(eOnJoin, NULL, NULL, CString(GetMyServer()), CString(GetMyNickName())+"!"+GetMyIdent(), pDoc->m_proto->m_strChannel, CString(""));
						break;
					}
					default:
						ASSERT(FALSE);
				}
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,128,0), 4);
			break;
		}

		case RPL_ENDOFNAMES:	// 366
		{
			// Get oldest queued ctNames query object
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(ctNames, &pos);

			if (pQuery)
			{
				ASSERT(pos);
				switch (pQuery->GetQueryPurpose())
				{
					case qpInitialNames:
						ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
						break;
					default:
						ASSERT(FALSE);
				}
				m_queries.FreeRemoveAt(pos);
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,128,0), 3, TRUE);
			break;
		}

		case RPL_WHOREPLY:		// 352
		{
			if (pParse->nArgs >= 8)
			{
				// Get oldest queued ctWho query object
				CCQuery*	pQuery = m_queries.FindQuery(ctWho, NULL);

				if (pQuery)
				{
					switch (pQuery->GetQueryPurpose())
					{
						case qpOnConnectEvent:
						case qpOnDisconnectEvent:
						case qpOnNotification:
						{
							// Make sure this server answer matches to the query's pPrUserMatch filter
							ASSERT(pQuery->GetPrUserMatch());
							if (bIsMatch(pQuery->GetPrUserMatch(), pParse->args[6], pParse->args[3], pParse->args[4]))
							{
								CCRule*		pRule = NULL;
								CCNotif*	pNotif = NULL;

								if (qpOnNotification == pQuery->GetQueryPurpose())
								{
									pNotif = (CCNotif*) pQuery->GetData();
									ASSERT(pNotif);
								}
								else
								{
									pRule = (CCRule*) pQuery->GetData();
									ASSERT(pRule);
								}

								if ((pRule && pRule->bActive() && !pRule->bStopped()) ||
									(pNotif && pNotif->bActive()))
								{
									CCDaemonExt* pDaemonExt = pRule ? pRule->GetDaemonExt() : pNotif->GetDaemonExt();
									ASSERT(pDaemonExt);
									CUser* pUser = CreateUserFromWhoReply(pParse);
									if (pUser)
									{
										if (!pDaemonExt->bAddUserToCurrentList(pUser))
											pUser->Release();
									}
								}
							}
							break;
						}

						case qpInitialWho:
						case qpUserListDlg:
						{
							CSInString(&pParse->args[3]);

							if (qpInitialWho == pQuery->GetQueryPurpose())
								UpdateIgnoreOnEntry(pParse->args[2], pParse->args[6], pParse->args[3], pParse->args[4]);
							else
							{
								CUser* pUser = CreateUserFromWhoReply(pParse);
								if (pUser)
									AddToUserList(pUser);
							}
							break;
						}

						default:
							ASSERT(FALSE);
					}
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
					pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,128,128), 3);
			}
			break;
		}

		case RPL_ENDOFWHO:		// 315
		{
			// Get oldest queued ctWho query object
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(ctWho, &pos);

			if (pQuery)
			{
				ASSERT(pos);
				m_queries.RemoveAt(pos);
				switch (pQuery->GetQueryPurpose())
				{
					case qpOnConnectEvent:
					case qpOnDisconnectEvent:
					case qpOnNotification:
					{
						CCRule*		pRule = NULL;
						CCNotif*	pNotif = NULL;

						if (qpOnNotification == pQuery->GetQueryPurpose())
						{
							pNotif = (CCNotif*) pQuery->GetData();
							ASSERT(pNotif);
						}
						else
						{
							pRule = (CCRule*) pQuery->GetData();
							ASSERT(pRule);
						}

						if ((pRule && pRule->bActive() && !pRule->bStopped()) ||
							(pNotif && pNotif->bActive()))
						{
							CCDaemonExt* pDaemonExt = pRule ? pRule->GetDaemonExt() : pNotif->GetDaemonExt();
							ASSERT(pDaemonExt);
							if (pRule)
								VERIFY(pDaemonExt->bOnEndOfListing(&(theApp.m_dynaRules), pRule, pQuery->GetQueryPurpose()));
							else
								VERIFY(pDaemonExt->bOnEndOfListing(&(theApp.m_dynaNotifs), pNotif));
						}
						break;
					}
					case qpInitialWho:
					case qpUserListDlg:
					{
						if (qpUserListDlg == pQuery->GetQueryPurpose())
							EndUserList();
						break;
					}
					default:
						ASSERT(FALSE);
				}
				delete pQuery;
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,128,128), 3, TRUE);
			break;
		}

		case RPL_INFO:			// 371
		case RPL_ENDOFINFO:		// 374
		case RPL_VERSION:		// 351
		case RPL_TIME:			// 391
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(128,0,0), 3, pParse->uCode != RPL_INFO);
			break;
		}

		case RPL_BANLIST:		// 367
		{
			// <channel> <banid>
			if (pParse->nArgs >= 4)
			{
				g_arrayBans.Add(DecodeNick(pParse->args[3]));
				pIrcPrint->SetFormat(PT_NONE);
			}
			break;
		}

		case RPL_ENDOFBANLIST:	// 368
		{
			// <channel> :End of channel ban list

			// Get oldest queued ctSetChannelMode query object
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(ctSetChannelMode, &pos);

			if (pQuery)
			{
				ASSERT(qpComSetChannelMode == pQuery->GetQueryPurpose());
				ASSERT(0 == stricmp(pQuery->GetChannelName(), pParse->args[2]));
				ASSERT(pos);
				m_queries.FreeRemoveAt(pos);
			}

			pIrcPrint->SetFormat(PT_NONE);
			DoBanDlg(pParse->args[2], g_strBan, g_arrayBans);
			g_strBan = "";
			g_arrayBans.RemoveAll();
			break;
		}

		case RPL_MOTDSTART:		// 375
		{
			pIrcPrint->SetFormat(PT_NONE);
			break;
		}

		case RPL_MOTD:			// 372
		case RPL_MOTD2:			// 377
		{
			// 377 used by irc.sprynet.com
			const char *szMOTD = pParse->lastString;

			if (szMOTD)
			{
				if (strncmp(szMOTD, "- ", 2) == 0)
					szMOTD += 2;
				if (strcmp(szMOTD, "-") == 0)
					szMOTD++;
				m_strMOTD += szMOTD;
				m_strMOTD += "\r\n";
				strLine = szMOTD;
				strLine += "\r";
			}

			CCQuery* pQuery = m_queries.FindQuery(ctLUsersMOTD, NULL);
			if (pQuery && pQuery->GetQueryPurpose() == qpLUsersMOTD)
				pIrcPrint->SetFormat(PT_NONE);
			else
				pIrcPrint->SetFormat(PT_WHOLESTRING, strLine, RGB(0,128,0));
			break;
		}

		case RPL_ENDOFMOTD:		// 376
		{
			// Get oldest queued ctLUsersMOTD query object
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(ctLUsersMOTD, &pos);
			BOOL		bNewLine = (!pQuery || pQuery->GetQueryPurpose() == qpInitialLUsersMOTD);

			if (pQuery)
			{
				if ((pQuery->GetQueryPurpose() == qpLUsersMOTD || (theApp.m_flags1 & F1_SHOWMOTD)) && (!m_strMOTD.IsEmpty() || !m_strLUSER.IsEmpty()))
					ShowMOTD(m_strLUSER, m_strMOTD);

				ASSERT(pos);
				m_queries.FreeRemoveAt(pos);
			}

			m_strMOTD = "";
			m_strLUSER = "";
			pIrcPrint->SetFormat(PT_NONE, "", RGB(0,128,0), 0, bNewLine);
			theApp.m_bDisableMOTD = FALSE;
			break;
		}

		case RPL_YOUREOPER:		// 381
		case RPL_YOUREADMIN:	// 386
		{
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,255), 2, TRUE);
			break;
		}

		case RPL_IRCX:			// 800
		{
			// Prefix		 = "keezer"
			// pParse->args[0] = "800"
			// pParse->args[1] = "*"						// no nickname specified yet
			// pParse->args[2] = "0|1"					// we asked to turn into the IRCX mode or not
			// pParse->args[3] = "0"						// IRCX version number
			// pParse->args[4] = "NTLM,ANON"				// security packages available
			// pParse->args[5] = "512"					// max message length
			// pParse->args[6] = "*"

			// ASSERT('*' == pParse->args[1][0]);
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(ctModeIsIrcX, &pos);

			if (!pQuery)
				pQuery = m_queries.FindQuery(ctIrcX, &pos);

			if (pQuery)
			{
				ASSERT(pos);
				m_queries.FreeRemoveAt(pos);

				// is this the first instance of 800 or the second?
				if ('0' == pParse->args[2][0])
				{
					// first instance, we're still in IRC mode
					m_bIrcXServer = TRUE;

					m_bJustSentModeIsIrcX = FALSE;

					// Retain only the legacy anonymous-login signal. Package-based
					// SSPI authentication was replaced by SASL in the portable core.
					if (pParse->nArgs >= 7)
					{
						const char *package = pParse->args[4];
						while (*package)
						{
							const char *end = strchr(package, ',');
							const std::size_t length = end
								? static_cast<std::size_t>(end - package)
								: strlen(package);
							if (length == strlen(g_szAnon) && !strnicmp(package, g_szAnon, length))
								m_bAnonAllowed = TRUE;
							if (!end)
								break;
							package = end + 1;
						}
					}

					// Read the max message length
					SHORT nMaxMsgLength = atoi(pParse->args[pParse->nArgs-2]);

					// Is the max length bigger than our current buffers?
					if (m_nMaxMsgLength < nMaxMsgLength)
					{
						HrInitAlloc(nMaxMsgLength);
						theApp.HrAllocBuffer(nMaxMsgLength);
					}

					// REGISB: revisit since HrInitAlloc might return OOM

					// finally switch to IRCX mode
					GetIrcProto()->bExecuteQuery(qpIrcX, ctIrcX, dtMax, NULL, "", "");
				}
				else
				{
					// second instance, we're already in IRCX mode
					// Login Time
					HrIrcXLogin(TRUE /*bForceNextPackage*/);
				}
				pIrcPrint->SetFormat(PT_NONE);
			}
			else
				pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 3, TRUE);
			break;
		}

		case RPL_ACCESSADD:		// 801
		case RPL_ACCESSDELETE:	// 802
		case RPL_ACCESSSTART:	// 803
		case RPL_ACCESSLIST:	// 804
		case RPL_ACCESSEND:		// 805
		case RPL_EVENTADD:		// 806
		case RPL_EVENTDEL:		// 807
		case RPL_EVENTSTART:	// 808
		case RPL_EVENTLIST:		// 809
		case RPL_EVENTEND:		// 810
			pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 3, pParse->uCode != RPL_ACCESSLIST && pParse->uCode == RPL_EVENTLIST);
			break;

		case RPL_PROPLIST:		// 818
		{
			if (pParse->nArgs >= 4)
			{
				// Get oldest queued ctPropGet query object
				CCQuery*	pQuery = m_queries.FindQuery(ctPropGet, NULL);

				if (pQuery)
				{
					ASSERT(GetIrcProto());
					switch (pQuery->GetQueryPurpose())
					{
						case qpJoinPics:
						case qpCreatePics:
						{
							pQuery->SetQueryPurpose(qpMax);
							CC_ASSERT(0 == lstrcmpi(pParse->args[3], "PICS"),
								"PICS property response expected");
							if (bPassesRatings(pParse->lastString, TRUE))
							{
								CRoomInfo* pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) pParse->args[2]);
								if (qpJoinPics == pQuery->GetQueryPurpose())
									GetIrcProto()->ChatJoinAux(*pEnterInfo);
								else
									GetIrcProto()->ChatCreateAux(*pEnterInfo);
							}
							// REGISB FIX ME ?? test it!!
							// else
							//	display error message PICS ratings don't allow you to step into
							break;
						}
						case qpJoinBackUrl:
						{
							CC_ASSERT(0 == lstrcmpi(pParse->args[3], "CLIENT"),
								"CLIENT property response expected");
							GetIrcProto()->HandleClientDataChange (pParse->lastString);
							break;
						}
						default:
							ASSERT(FALSE);
					}
					pIrcPrint->SetFormat(PT_NONE);
				}
				else
					pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(0,0,0), 3, TRUE);
			}
			break;
		}

		case RPL_PROPEND:		// 819
		{
			if (pParse->nArgs >= 3)
			{
				// Get oldest queued ctPropGet query object
				POSITION	pos;
				CCQuery*	pQuery = m_queries.FindQuery(ctPropGet, &pos);

				if (pQuery)
				{
					ASSERT(GetIrcProto());
					ASSERT(pos);
					m_queries.RemoveAt(pos);
					switch (pQuery->GetQueryPurpose())
					{
						case qpJoinPics:
						case qpCreatePics:
						{
							if (bCanViewUnrated(TRUE))
							{
								CRoomInfo* pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) pParse->args[2]);
								if (qpJoinPics == pQuery->GetQueryPurpose())
									GetIrcProto()->ChatJoinAux(*pEnterInfo);
								else
									GetIrcProto()->ChatCreateAux(*pEnterInfo);
							}
							break;
						}
						case qpJoinBackUrl:
						case qpMax:
							break;
						default:
							ASSERT(FALSE);
					}
					delete pQuery;
				}
				pIrcPrint->SetFormat(PT_NONE);
			}
			break;
		}
	}
}


void CIrcSocket::HandleErrorCode(char *szLine, PIRCPARSE pParse, CIrcPrint *pIrcPrint)
{
	CString		strMesg;
	BOOL		bDisplayErrorInStatusWindow = FALSE;
	LPCSTR		szChannelName = NULL;
	INT			iRoomIndex = -1;
	CRoomInfo*	pEnterInfo = NULL;

	ASSERT(pParse);
	ASSERT(pIrcPrint);
	ASSERT(pParse->uCode);

	switch (pParse->uCode)
	{
		default:
		{
			#ifdef DEBUG
				TRACE("Untreated error: sender nick = %s, mach = %s, user = %s, error = %s, last string = %s\n",
				pParse->nick ? pParse->nick : "",
				pParse->machine ? pParse->machine : "",
				pParse->user ? pParse->user : "",
				pParse->args[0] ? pParse->args[0] : "",
				pParse->lastString ? pParse->lastString : "");
			#endif // DEBUG
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_NOSUCHNICK:
		{
			//:chloe1 401 <mynick> <nick|channel> :No such nick/channel
			if (CHANNELPREFIX(pParse->args[2][0]))
			{
				// No such channel
				strMesg.Format(IDS_ERR_NOSUCHCHANNEL, DecodeChan(pParse->args[2]));	// don't know if it's MIC
			}
			else
			{
				// No such nickname
				strMesg.Format(IDS_ERR_NOSUCHNICK, DecodeNick(pParse->args[2]));
				bFreeModeCell(NULL, pParse->args[2]);
			}
			AfxMessageBox(strMesg);
			break;
		}

		case ERR_NOSUCHCHANNEL:		// 403
		{
			if (pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) pParse->args[2], &iRoomIndex, FALSE /*bCloneOK*/, TRUE /*bNullOK*/))
				ShowBadChannelName(pParse->nArgs > 2 ? pParse->args[2] : "");
			else
			{
				POSITION	pos;
				CCQuery*	pQuery = m_queries.FindQuery(ctTopic, &pos);

				iRoomIndex = 0;

				if (pQuery && qpListMembers == pQuery->GetQueryPurpose())
				{
					ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
					ASSERT(pos);
					m_queries.FreeRemoveAt(pos);

					strMesg.Format(IDS_ERR_NOSUCHCHANNELANYMORE, DecodeChan(pParse->args[2]));	// don't know if it's MIC

					// we need to re-enable the button
					if (cui.m_pvRoomList)
					{
						CRoomList*	pRoomList = (CRoomList*) cui.m_pvRoomList;
						CWnd*		pBtn1 = pRoomList->GetDlgItem(IDC_RESET_LIST);
						CWnd*		pBtn2 = pRoomList->GetDlgItem(IDC_LISTMEMBERS);

						if (pBtn1 && pBtn2)
						{
							pBtn2->EnableWindow(TRUE);
							pRoomList->GotoDlgCtrl(pBtn1);
							pRoomList->NextDlgCtrl();
						}
					}
				}
				else
				{
					strMesg.Format(IDS_ERR_NOSUCHCHANNEL, DecodeChan(pParse->args[2]));	// don't know if it's MIC
					bFreeModeCell(pParse->args[2], pParse->args[2]);
				}
				AfxMessageBox(strMesg);
			}
			break;
		}

		case ERR_TOOMANYCHANNELS:	// 405
		{
			AfxMessageBox(IDS_ERR_TOOMANYCHANNELS);
			break;
		}

		case ERR_NOMOTD:			// 422
		{
			ASSERT(m_strMOTD.IsEmpty());

			strMesg.LoadString(IDS_ERR_NOMOTD);
			pIrcPrint->SetFormat(PT_WHOLESTRING, strMesg, RGB(0,0,255), 0, TRUE);
			AddToStatus(*pIrcPrint, strMesg, NULL);

			// Get oldest queued ctLUsersMOTD query object
			POSITION pos;
			CCQuery* pQuery = m_queries.FindQuery(ctLUsersMOTD, &pos);

			if (pQuery)
			{
				if ((pQuery->GetQueryPurpose() == qpLUsersMOTD || (theApp.m_flags1 & F1_SHOWMOTD)) && !m_strLUSER.IsEmpty())
					ShowMOTD(m_strLUSER, "");

				ASSERT(pos);
				m_queries.FreeRemoveAt(pos);
			}
			m_strLUSER = "";
			theApp.m_bDisableMOTD = FALSE;

			break;
		}

		case ERR_NONICKNAMEGIVEN:	// 431
		case ERR_ERRONEUSNICKNAME:	// 432
		case ERR_NICKNAMEINUSE:		// 433
		{
			int		iIndex = (ERR_NICKNAMEINUSE == pParse->uCode) ? 2 : 1;
			const char*	szBadNick = pParse->nArgs >= (iIndex+1) ? pParse->args[iIndex] : "";
			GetIrcProto()->TryNewNick((ERR_NICKNAMEINUSE == pParse->uCode) ? ID_ERR_DUPED_NICK : ID_ERR_BAD_NICK, m_bIrcXServer ? DecodeNick(szBadNick) : szBadNick);
			break;
		}

		case ERR_NICKCOLLISION:		// 436
		{
			AfxMessageBox(IDS_ERR_NICKCOLLISION);
			break;
		}

		case ERR_NICKTOOFAST:		// 438
		{
			AfxMessageBox(IDS_ERR_NICKTOOFAST);
			break;
		}

		case ERR_NICKNOCHANGE:		// 439
		{
			AfxMessageBox(IDS_ERR_NICKNOCHANGE);
			break;
		}

		case ERR_NOTONCHANNEL:		// 442
		{
			// <channel> :You're not on that channel
			CCQuery*	pQuery;
			POSITION	pos;

			if (pQuery = m_queries.FindQuery(ctTopic, &pos))
			{
				// happens on irc.dal.net for example when trying to get the topic of a channel
				// in order to list its members.
				ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
				ASSERT(qpListMembers == pQuery->GetQueryPurpose());
				CString strEncodedChannel = pQuery->GetChannelName();
				CString strPrettyChannel  = pQuery->GetData() ? (LPTSTR) pQuery->GetData() : "";
				ASSERT(pos);
				m_queries.FreeRemoveAt(pos);
				OnUserListAux(NULL, strEncodedChannel, strPrettyChannel);
			}
			else
			{
				bFreeModeCell(pParse->args[2], NULL);
				bDisplayErrorInStatusWindow = TRUE;
			}
			break;
		}

		case ERR_NOTREGISTERED:		// 451
		{
			// :You have not registered
			HrModeIsIrcXFailure();
			break;
		}

		case ERR_NEEDMOREPARAMS:	// 461
		{
			// <command> :Not enough parameters
			bFreeModeCell(NULL, NULL);
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_PASSWDMISMATCH: 	// 464
		{
			// Wrong password for plaintext, for oper keyword.
			(void)HrIrcSetOper(m_userName.empty() ? GetMyUserName() : m_userName.c_str(), NULL);
			break;
		}

		case ERR_YOUREBANNEDCREEP:	// 465
		{
			AfxMessageBox(IDS_ERR_YOUREBANNEDCREEP);
			break;
		}

		case ERR_YOUWILLBEBANNED:	// 466
		{
			AfxMessageBox(IDS_ERR_YOUWILLBEBANNED);
			break;
		}

		case ERR_KEYSET:			// 467
		{
			// <channel> :Channel key already set
			bFreeModeCell(pParse->args[2], NULL);
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_CHANNELISFULL:		// 471
		{
			strMesg.Format(ID_ERR_CHANNELISFULL, DecodeChan(pParse->args[2]));	// don't know if it's MIC
			AfxMessageBox(strMesg);
			break;
		}

		case ERR_UNKNOWNMODE:		// 472
		{
			// <char> :is unknown mode char to me
			bFreeModeCell(NULL, NULL);
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_INVITEONLYCHAN:	// 473
		{
			strMesg.Format(ID_ERR_INVITEONLY, DecodeChan(pParse->args[2]));	// don't know if it's MIC
			AfxMessageBox(strMesg);
			break;
		}

		case ERR_BANNEDFROMCHAN:	// 474
		{
			strMesg.Format(ID_ERR_BANNEDFROMCHAN, DecodeChan(pParse->args[2]));	// don't know if it's MIC
			AfxMessageBox(strMesg);
			break;
		}

		case ERR_BADCHANNELKEY:		// 475
		{
			pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) pParse->args[2], &iRoomIndex);
			OnBadChannelPassword(*pEnterInfo);
			break;
		}

		case ERR_CHANOPRIVSNEEDED:	// 482
		{
			// <channel> :You're not channel operator
			if (!bFreeModeCell(pParse->args[2], NULL))
			{
				// Does this come from a TOPIC command that failed?
				// Get oldest queued ctTopic query object
				POSITION	pos;
				CCQuery*	pQuery = m_queries.FindQuery(ctTopic, &pos);

				if (pQuery)
				{
					// this should not happen because UI does not allow this situation
					ASSERT(pos);
					switch (pQuery->GetQueryPurpose())
					{
						case qpSetTopic:
							ASSERT(0 == strcmp(pParse->args[2], pQuery->GetChannelName()));
							break;
						default:
							ASSERT(FALSE);
					}
					m_queries.FreeRemoveAt(pos);
				}
			}
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_UMODEUNKNOWNFLAG:	// 501
			// :Unknown MODE flag
		case ERR_USERSDONTMATCH:	// 502
			// :Can't change mode for other users
		{
			bFreeModeCell(NULL, "");
			bDisplayErrorInStatusWindow = TRUE;
			break;
		}

		case ERR_NOJOINDYNAMIC:		// 552
		{
			AfxMessageBox(IDS_ERR_NOJOINDYNAMIC);
			break;
		}

		case ERR_NODYNAMICCHANNELS:	// 553
		{
			AfxMessageBox(IDS_ERR_NODYNAMICCHANNELS);
			break;
		}

		case ERR_AUTHONLY:			// 556
		{
			AfxMessageBox(IDS_ERR_AUTHONLY);
			break;
		}

		case ERR_CANNOTCREATEDYNAMIC:	// or ERR_BADFUNCTION 902
		{
			if (!m_bIrcXServer)
			{
				// ERR_CANNOTCREATEDYNAMIC case
				AfxMessageBox(IDS_ERR_NODYNAMICCHANNELS);
			}
			// else ERR_BADFUNCTION case not treated
			break;
		}

		case ERR_ONLYAUTHCANJOIN:	// or ERR_BADTAG 904
		{
			if (!m_bIrcXServer)
			{
				// ERR_ONLYAUTHCANJOIN case
				AfxMessageBox(IDS_ERR_AUTHONLY);
			}
			// else ERR_BADTAG case not treated
			break;
		}

		case ERR_CANNOTCHANGENICK:	// or ERR_BADPROPERTY 905
		{
			if (!m_bIrcXServer)
			{
				// ERR_CANNOTCHANGENICK case
				AfxMessageBox(IDS_ERR_NICKNOCHANGE);
			}
			// else ERR_BADPROPERTY case not treated
			break;
		}

		case ERR_CANNOTJOINDYNAMIC:		// or ERR_RESOURCE 907
		{
			if (!m_bIrcXServer)
			{
				// ERR_CANNOTJOINDYNAMIC case
				AfxMessageBox(IDS_ERR_NOJOINDYNAMIC);
			}
			// else ERR_RESOURCE case not treated
			break;
		}

		case ERR_AUTHENTICATIONFAILED:	// 910
		{
			// Legacy IRCX SSPI authentication is no longer retried. SASL failures
			// use their standard 904-908 numerics in the shared protocol engine.
			AfxMessageBox(ID_ERR_BADUSERINFO);
			break;
		}

		case ERR_UNKNOWNPACKAGE:	// 912
		{
			// ":<servername> 912 * <secupackage> : Unsupported authentication package
			AfxMessageBox(ID_ERR_BADUSERINFO);
			break;
		}

		case ERR_NOSUCHOBJECT:		// 924
		{
			// Get oldest queued ctPropGet query object
			POSITION	pos;
			CCQuery*	pQuery = m_queries.FindQuery(ctPropGet, &pos);

			if (pQuery)
			{
				ASSERT(GetIrcProto());
				switch (pQuery->GetQueryPurpose())
				{
					case qpJoinPics:
					case qpCreatePics:
					{	// prop test on non-existant room
						if (!pQuery->GetChannelName().Compare(pParse->args[2]) && bCanViewUnrated(TRUE))
						{
							pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) pParse->args[2]);
							if (qpJoinPics == pQuery->GetQueryPurpose())
								GetIrcProto()->ChatJoinAux(*pEnterInfo);
							else
								GetIrcProto()->ChatCreateAux(*pEnterInfo);
						}
						ASSERT(pos);
						m_queries.FreeRemoveAt(pos);
						break;
					}
				}
			}
			else
				bDisplayErrorInStatusWindow = TRUE;
			break;
		}
	}

	// Set the szChannelName variable
	switch (pParse->uCode)
	{
		// IRC/IRCX common errors
		case ERR_USERONCHANNEL:
			// <nick> <channel> :is already on channel
			szChannelName = pParse->args[3];
			break;

		case ERR_NOSUCHNICK:
			// <nick/channel> :No such nick/channel
			szChannelName = pParse->args[2];
			break;

		case ERR_INVITEONLYCHAN:
			// <channel> :Cannot join channel (+i)
		case ERR_CHANNELISFULL:
			// <channel> :Cannot join channel (+l)
		case ERR_NOSUCHCHANNEL:
			// <channel> :No such channel
		case ERR_BANNEDFROMCHAN:
			// <channel> :Cannot join channel (+b)
		case ERR_BADCHANNELKEY:
			// <channel> :Cannot join channel (+k)
		case ERR_TOOMANYCHANNELS:
			// <channel> :You have joined too many channels
			szChannelName = pParse->args[2];
	}

	if (m_bIrcXServer)
		switch (pParse->uCode)
		{
			// IRCX only errors
			case ERR_NOJOINDYNAMIC:
				// <Channel> :Cannot join dynamic channels due to admin restriction
			case ERR_NODYNAMICCHANNELS:
				// <Channel> :Cannot create dynamic channels due to admin restriction
			case ERR_AUTHONLY:
				// <Channel> :Only authenticated users may join channel
			case ERR_CHANNELEXIST:
				// <Channel> :Channel already exists.
				szChannelName = pParse->args[2];
				break;

			case ERR_NOACCESS:
				// <*|ChannelName> :No access
				if (_tcscmp("*", pParse->args[2]))
					szChannelName = pParse->args[2];
				break;
		}
	else
		switch (pParse->uCode)
		{
			// MIC 1.0 errors
			case ERR_CANNOTJOINMICONLY:
				// <channel> :Cannot join MIC only channel with IRC client
			case ERR_CANNOTJOINFROMREMOTE:
				// <channel> :Cannot join channel from remote server (+r)
			case ERR_CANNOTCREATEDYNAMIC:
				// <channel> :Cannot create dynamic channels (admin)
			case ERR_ONLYAUTHCANJOIN:
				// <channel> :Only authenticated users may join channel
			case ERR_CANNOTJOINDYNAMIC:
				// <channel> :Cannot join dynamic channels due to admin restriction"
				szChannelName = pParse->args[2];
		}

	if (szChannelName)
	{
		// Try to find the opening channel in our list
		if (-1 == iRoomIndex)
			pEnterInfo = theApp.GetRoomInfoFromName((LPCTSTR) szChannelName, &iRoomIndex, FALSE /*bCloneOK*/);

		if (pEnterInfo && iRoomIndex > 0)
			theApp.RemoveRoomInfo(iRoomIndex);
	}

	if (bDisplayErrorInStatusWindow)
		pIrcPrint->SetFormat(PT_OFFSET, szLine, RGB(255,0,0), 3, TRUE);
	else
		pIrcPrint->SetFormat(PT_NONE);
}


BOOL CIrcSocket::bFreeModeCell(LPCTSTR szChannel, LPCTSTR szNickname)
{
	// is there a queued ctSetChannelMode or ctSetUserMode cell with query purpose == qpComSetChannelMode or qpComSetUserMode?

	// Get oldest queued ctSetUserMode and ctSetChannelMode query object
	POSITION	pos1, pos2;
	LONG		lRank1, lRank2;
	CCQuery*	pQuery1 = NULL;
	CCQuery*	pQuery2 = NULL;

	if (szNickname || (!szNickname && !szChannel))
		pQuery1 = m_queries.FindQuery(ctSetUserMode, &pos1, &lRank1);

	if (szChannel || (!szNickname && !szChannel))
		pQuery2 = m_queries.FindQuery(ctSetChannelMode, &pos2, &lRank2);

	if (!pQuery1 || qpComSetUserMode != pQuery1->GetQueryPurpose())
		lRank1 = 0L;

	if (!pQuery2 || qpComSetChannelMode != pQuery2->GetQueryPurpose())
		lRank2 = 0L;

	if (lRank1 && (!lRank2 || lRank1 < lRank2))
	{
		ASSERT(pos1);
		m_queries.FreeRemoveAt(pos1);
		return TRUE;
	}
	if (lRank2 && (!lRank1 || lRank2 < lRank1))
	{
		ASSERT(pos2);
		m_queries.FreeRemoveAt(pos2);
		return TRUE;
	}
	return FALSE;
}


void
CIrcSocket::SetAuthentication(
UINT   nType,
LPCSTR pszUserName,
LPCSTR pszPassword,
LPCSTR pszCustomPkg)
{
	(void)pszCustomPkg;
	m_nAuthenticationType = nType;
	m_bAnonAllowed = FALSE;
	SecureClear(&m_userName);
	m_password.clear();
	if (pszUserName)
		m_userName = pszUserName;
	if (pszPassword && !StorePassword(pszPassword)) {
		SecureClear(&m_userName);
		m_nAuthenticationType = authtypeNone;
	}
}
