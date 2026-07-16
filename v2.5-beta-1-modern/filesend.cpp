// filesend.cpp : bounded, event-driven legacy DCC file transfer adapter
//

#include "stdafx.h"
#include "chat.h"
#include "filesend.h"
#include "Process.H"
#include "UserInfo.H"
#include "ChatProt.H"
#include "UI.H"
#include "cderr.h"
#include "ircproto.h"
#include "ircsock.h"
#include "protsupp.h"
#include "comicchat/net/dcc_transfer_engine.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

extern CChatApp theApp;

struct FILETXINFO final {
	FILETXINFO();
	~FILETXINFO();

	FILETXINFO(const FILETXINFO&) = delete;
	FILETXINFO& operator=(const FILETXINFO&) = delete;

	CFileProgress *progDlg = NULL;
	comic_chat::net::DccTransferEngine engine;
	comic_chat::net::DccTransferHandle handle{};
	std::string recipient;
	std::string quotedFileName;
	std::optional<std::string> expectedPeerAddress;
	char pathName[MAX_PATH]{};
	std::uint64_t fileSize = 0;
	std::uint64_t fileOffset = 0;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	BOOL active = FALSE;
	BOOL sending = FALSE;
	BOOL offerSent = FALSE;
};

#define WM_STATCHANGE       (WM_USER + 1)
#define WP_CONNECTED        1
#define WP_FILESENT         2
#define WP_BYTESSENT        3
#define WP_CONNECTFAILED    4
#define WP_TIMEOUT          5
#define WP_NETWORK_EVENT    6

namespace {

constexpr std::size_t kMaximumEventBatch = 128;
constexpr std::size_t kMaximumEventBatchesPerWake = 8;
constexpr std::size_t kFileChunkBytes = 16U * 1024U;
constexpr std::uint64_t kMaximumDccFileBytes = 0xffff'ffffULL;
constexpr INT_PTR kMaximumActiveFileTransfers = 8;
constexpr INT_PTR kMaximumStoredFileTransfers = 32;

struct DccOffer final {
	std::string fileName;
	std::string peerAddress;
	std::uint16_t port{};
	std::uint64_t fileSize{};
};

bool IsSendCommand(std::string_view value)
{
	if (value.size() != 4)
		return false;
	constexpr std::string_view command{"SEND"};
	for (std::size_t index = 0; index < command.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(value[index]);
		const char folded = byte >= 'a' && byte <= 'z' ? static_cast<char>(byte - ('a' - 'A')) : value[index];
		if (folded != command[index])
			return false;
	}
	return true;
}

CString UInt64Text(std::uint64_t value)
{
	char buffer[32]{};
	auto converted = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value);
	if (converted.ec != std::errc{})
		return CString("0");
	*converted.ptr = '\0';
	return CString(buffer);
}

template <typename Integer>
std::optional<Integer> ParseUnsigned(std::string_view value, Integer maximum)
{
	if (value.empty())
		return std::nullopt;
	Integer parsed{};
	const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
	if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || parsed > maximum)
		return std::nullopt;
	return parsed;
}

std::vector<std::string_view> SplitOffer(std::string_view value)
{
	if (!value.empty() && value.back() == 0x01)
		value.remove_suffix(1);
	std::vector<std::string_view> tokens;
	std::size_t cursor = 0;
	while (cursor < value.size()) {
		while (cursor < value.size() && value[cursor] == ' ')
			++cursor;
		if (cursor == value.size())
			break;
		const auto end = value.find(' ', cursor);
		tokens.push_back(value.substr(cursor, end - cursor));
		if (tokens.size() > 5)
			return {};
		cursor = end == std::string_view::npos ? value.size() : end + 1;
	}
	return tokens;
}

std::optional<std::string> SanitizeRemoteFileName(std::string value)
{
	if (value.empty())
		return std::nullopt;

	std::size_t base = 0;
	const char* begin = value.c_str();
	const char* cursor = begin;
	while (*cursor) {
		const char* next = CharNext(cursor);
		if (next - cursor == 1 && (*cursor == '\\' || *cursor == '/' || *cursor == ':'))
			base = static_cast<std::size_t>(next - begin);
		cursor = next;
	}
	value.erase(0, base);
	if (value.empty() || value == "." || value == "..")
		return std::nullopt;

	std::string sanitized;
	sanitized.reserve(std::min(value.size(), static_cast<std::size_t>(MAX_PATH - 1)));
	cursor = value.c_str();
	while (*cursor && sanitized.size() < MAX_PATH - 1) {
		const char* next = CharNext(cursor);
		if (next - cursor > 1) {
			const auto count = static_cast<std::size_t>(next - cursor);
			if (sanitized.size() + count >= MAX_PATH)
				break;
			sanitized.append(cursor, count);
		} else {
			const unsigned char byte = static_cast<unsigned char>(*cursor);
			if (byte < 0x20 || std::strchr("<>\"|?*:/\\", *cursor))
				sanitized.push_back('_');
			else
				sanitized.push_back(*cursor);
		}
		cursor = next;
	}
	while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.'))
		sanitized.pop_back();
	if (sanitized.empty() || sanitized == "." || sanitized == "..")
		return std::nullopt;
	return sanitized;
}

std::optional<std::string> LegacyAddressString(std::uint32_t address)
{
	char buffer[16]{};
	if (!TryFormatBuffer(buffer, sizeof(buffer), "%u.%u.%u.%u",
			static_cast<unsigned>((address >> 24) & 0xffU),
			static_cast<unsigned>((address >> 16) & 0xffU),
			static_cast<unsigned>((address >> 8) & 0xffU),
			static_cast<unsigned>(address & 0xffU)))
		return std::nullopt;
	if (!comic_chat::net::dcc_legacy_ipv4_decimal(buffer))
		return std::nullopt;
	return std::string(buffer);
}

std::optional<DccOffer> ParseDccOffer(const char* message)
{
	if (!message)
		return std::nullopt;
	const auto tokens = SplitOffer(message);
	if (tokens.size() != 5 || !IsSendCommand(tokens[0]))
		return std::nullopt;

	std::string quotedName(tokens[1]);
	const char* unquoted = quotedName.c_str();
	const BOOL freeUnquoted = CTCPUnQuoteString(&unquoted);
	auto fileName = SanitizeRemoteFileName(unquoted ? std::string(unquoted) : std::string{});
	if (freeUnquoted)
		free(const_cast<char*>(unquoted));
	if (!fileName)
		return std::nullopt;

	const auto address = ParseUnsigned<std::uint32_t>(tokens[2], std::numeric_limits<std::uint32_t>::max());
	const auto port = ParseUnsigned<std::uint32_t>(tokens[3], 65535U);
	const auto fileSize = ParseUnsigned<std::uint64_t>(tokens[4], kMaximumDccFileBytes);
	if (!address || !port || *port == 0 || !fileSize || *fileSize == 0)
		return std::nullopt;
	auto peerAddress = LegacyAddressString(*address);
	if (!peerAddress)
		return std::nullopt;

	return DccOffer{std::move(*fileName), std::move(*peerAddress),
		static_cast<std::uint16_t>(*port), *fileSize};
}

std::optional<std::string> NumericPeerAddress(CUserInfo* user)
{
	if (!user)
		return std::nullopt;
	const char* qualified = user->GetQualifiedName();
	const char* at = qualified ? std::strrchr(qualified, '@') : NULL;
	if (!at || !at[1])
		return std::nullopt;
	std::string address(at + 1);
	if (!comic_chat::net::dcc_legacy_ipv4_decimal(address))
		return std::nullopt;
	return address;
}

void CloseTransferFile(FILETXINFO* transfer)
{
	if (transfer && transfer->hFile != INVALID_HANDLE_VALUE) {
		CloseHandle(transfer->hFile);
		transfer->hFile = INVALID_HANDLE_VALUE;
	}
}

bool WriteAll(HANDLE file, const std::vector<std::byte>& bytes)
{
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		DWORD written = 0;
		const auto remaining = static_cast<DWORD>(bytes.size() - offset);
		if (!WriteFile(file, bytes.data() + offset, remaining, &written, NULL) || written == 0)
			return false;
		offset += static_cast<std::size_t>(written);
	}
	return true;
}

FileTransferError TransferErrorCategory(comic_chat::net::DccError error)
{
	switch (error) {
	case comic_chat::net::DccError::invalid_address:
		return FileTransferError::address_validation;
	case comic_chat::net::DccError::queue_full:
		return FileTransferError::resource_limit;
	case comic_chat::net::DccError::invalid_options:
	case comic_chat::net::DccError::stale_transfer:
	case comic_chat::net::DccError::protocol_error:
		return FileTransferError::protocol;
	case comic_chat::net::DccError::already_running:
	case comic_chat::net::DccError::not_running:
		return FileTransferError::connection;
	}
	return FileTransferError::protocol;
}

void InstallWakeup(FILETXINFO* transfer)
{
	if (!transfer || !transfer->progDlg)
		return;
	const HWND hwnd = transfer->progDlg->GetSafeHwnd();
	transfer->engine.set_wakeup([hwnd]() {
		if (hwnd)
			::PostMessage(hwnd, WM_STATCHANGE, WP_NETWORK_EVENT, 0);
	});
}

void ShowTransferCapacityMessage()
{
	AfxMessageBox("Too many file transfers are open. Close a completed transfer or cancel an active one.",
		MB_OK | MB_ICONEXCLAMATION);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// CFileProgress dialog

FILETXINFO::FILETXINFO() = default;

FILETXINFO::~FILETXINFO()
{
	engine.set_wakeup({});
	engine.stop();
	CloseTransferFile(this);
}

CFileProgress::CFileProgress(CWnd* pParent /*=NULL*/)
	: CDialog(CFileProgress::IDD, pParent)
{
	m_bytesSent = _T("");
	m_bytesTotal = _T("");
	m_strStatus = _T("");
	m_strXferredLabel = _T("");
	m_iBytesTotal = 0;
	m_iBytesTransferred = 0;
	m_bSending = FALSE;
	m_fileTX = NULL;
	m_transferPhase = FileTransferPhase::connecting;
	m_transferError = FileTransferError::none;
	m_pumpingTransferEvents = false;
	m_transferWakePending = false;
}

CFileProgress::~CFileProgress()
{
	delete m_fileTX;
	m_fileTX = NULL;
}

void CFileProgress::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_FILEPROGRESS, m_fileProgress);
	DDX_Text(pDX, IDC_BYTES_SENT, m_bytesSent);
	DDX_Text(pDX, IDC_BYTES_TOTAL, m_bytesTotal);
	DDX_Text(pDX, IDC_CX_STATUS, m_strStatus);
	DDX_Text(pDX, IDC_STATIC_NXFERRED, m_strXferredLabel);
}

BEGIN_MESSAGE_MAP(CFileProgress, CDialog)
	ON_MESSAGE(WM_STATCHANGE, OnStatChange)
END_MESSAGE_MAP()

static CPtrArray fileSendStore;

void CleanupFileProgressStore(BOOL bIncludeShowing)
{
	for (int index = fileSendStore.GetUpperBound(); index >= 0; --index) {
		auto* progress = static_cast<CFileProgress*>(fileSendStore[index]);
		if (!progress) {
			fileSendStore.RemoveAt(index);
			continue;
		}
		if (bIncludeShowing && progress->m_fileTX && progress->m_fileTX->active) {
			progress->m_fileTX->engine.set_wakeup({});
			progress->m_fileTX->engine.stop();
			progress->m_fileTX->active = FALSE;
		}
		if ((bIncludeShowing || !progress->m_hWnd || !progress->IsWindowVisible()) &&
			(!progress->m_fileTX || !progress->m_fileTX->active)) {
			delete progress;
			fileSendStore.RemoveAt(index);
			TRACE0("Cleaning up one file progress window.\n");
		}
	}
}

bool FileTransferCapacityAvailable()
{
	CleanupFileProgressStore(FALSE);
	INT_PTR active = 0;
	for (int index = 0; index <= fileSendStore.GetUpperBound(); ++index) {
		auto* progress = static_cast<CFileProgress*>(fileSendStore[index]);
		if (progress && progress->m_fileTX && progress->m_fileTX->active)
			++active;
	}
	return active < kMaximumActiveFileTransfers &&
		fileSendStore.GetSize() < kMaximumStoredFileTransfers;
}

bool AddToFileProgressStore(CFileProgress* progress)
{
	if (!progress || !FileTransferCapacityAvailable())
		return false;
	fileSendStore.Add(progress);
	ASSERT(fileSendStore.GetSize() <= kMaximumStoredFileTransfers);
	return true;
}

std::vector<FileTransferSnapshot> GetFileTransferSnapshots()
{
	ASSERT(AfxGetThread() == AfxGetApp());
	std::vector<FileTransferSnapshot> snapshots;
	const int upper = fileSendStore.GetUpperBound();
	if (upper >= 0)
		snapshots.reserve(static_cast<std::size_t>(upper) + 1);
	for (int index = 0; index <= upper; ++index) {
		auto* progress = static_cast<CFileProgress*>(fileSendStore[index]);
		if (progress)
			snapshots.push_back(progress->GetTransferSnapshot());
	}
	return snapshots;
}

BOOL FillInFilter(CFileDialog& dialog, char* buffer, UINT bufferSize)
{
	CString filter;
	filter.LoadString(IDS_ALL_FILES);
	if (!TryCopyBuffer(buffer, bufferSize, static_cast<LPCTSTR>(filter)))
		return FALSE;
	for (char* current = buffer; *current; ++current)
		if (*current == '\n')
			*current = '\0';
	dialog.m_ofn.lpstrFilter = buffer;
	dialog.m_ofn.nFilterIndex = 1;
	return TRUE;
}

void CRoomInfo::ChatSendFile(CUserInfo* user)
{
	if (!user || user->IsDeparted())
		return;
	if (!FileTransferCapacityAvailable()) {
		ShowTransferCapacityMessage();
		return;
	}
	const std::string localAddress = serverConn.GetLocalAddress();
	if (!comic_chat::net::dcc_legacy_ipv4_decimal(localAddress)) {
		AfxMessageBox(IDS_CONNECTION_FAILED);
		return;
	}

	CFileDialog dialog(TRUE);
	dialog.m_ofn.Flags |= OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	CString title;
	title.LoadString(IDS_TITLE_FILEDLG_SEND);
	VERIFY(ReplaceToken(title, CString("%1"), user->GetScreenName()));
	dialog.m_ofn.lpstrTitle = title;
	char filter[50]{};
	VERIFY(FillInFilter(dialog, filter, sizeof(filter)));
	if (theApp.DoModalDlg(&dialog) != IDOK)
		return;

	auto transfer = std::make_unique<FILETXINFO>();
	const CString pathName = dialog.GetPathName();
	if (!TryCopyArray(transfer->pathName, static_cast<LPCTSTR>(pathName))) {
		AfxMessageBox(ID_ERR_SAVE);
		return;
	}
	transfer->hFile = ::CreateFile(transfer->pathName, GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (transfer->hFile == INVALID_HANDLE_VALUE) {
		AfxMessageBox(ID_ERR_SAVE);
		return;
	}
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(transfer->hFile, &size) || size.QuadPart <= 0 ||
		static_cast<unsigned long long>(size.QuadPart) > kMaximumDccFileBytes) {
		AfxMessageBox("The selected file must be between 1 and 4,294,967,295 bytes.");
		return;
	}
	transfer->fileSize = static_cast<std::uint64_t>(size.QuadPart);
	transfer->recipient = static_cast<LPCTSTR>(user->GetName());
	transfer->expectedPeerAddress = NumericPeerAddress(user);
	CString fileName = GetFileNameFromFileDialog(dialog);
	auto sanitizedFileName = SanitizeRemoteFileName(static_cast<LPCTSTR>(fileName));
	if (!sanitizedFileName)
		return;
	fileName = sanitizedFileName->c_str();
	const char* quoted = sanitizedFileName->c_str();
	const BOOL freeQuoted = CTCPQuoteString(&quoted);
	transfer->quotedFileName = quoted ? quoted : "";
	if (freeQuoted)
		free(const_cast<char*>(quoted));
	if (transfer->quotedFileName.empty())
		return;

	auto* progress = new CFileProgress;
	if (!progress)
		return;
	transfer->progDlg = progress;
	transfer->sending = TRUE;
	transfer->active = TRUE;
	progress->m_fileTX = transfer.get();
	progress->m_bytesTotal = UInt64Text(transfer->fileSize);
	progress->m_iBytesTotal = transfer->fileSize;
	progress->m_strOtherGuy = user->GetScreenName();
	progress->m_strFileName = fileName;
	progress->m_strStatus.LoadString(IDS_AWAITING_ACCEPT);
	progress->m_strXferredLabel.LoadString(IDS_BYTES_SENT);
	progress->m_bSending = TRUE;
	progress->m_transferPhase = FileTransferPhase::awaiting_peer;
	VERIFY(ReplaceToken(progress->m_strStatus, CString("%1"), progress->m_strOtherGuy));
	if (!progress->Create(IDD_FILE_TRANSFER)) {
		progress->m_fileTX = NULL;
		delete progress;
		return;
	}
	progress->UpdateProgress(0);
	if (!AddToFileProgressStore(progress)) {
		progress->DestroyWindow();
		progress->m_fileTX = NULL;
		delete progress;
		ShowTransferCapacityMessage();
		return;
	}
	progress->ShowWindow(SW_SHOWNORMAL);
	FILETXINFO* activeTransfer = transfer.release();
	InstallWakeup(activeTransfer);

	comic_chat::net::DccListenOptions options;
	options.bind_address = localAddress;
	options.advertise_address = localAddress;
	options.expected_peer_address = activeTransfer->expectedPeerAddress;
	options.file_size = activeTransfer->fileSize;
	options.limits.receive_chunk_bytes = kFileChunkBytes;
	auto started = activeTransfer->engine.start_listen(std::move(options));
	if (!started) {
		progress->FinishTransfer(FALSE, TransferErrorCategory(started.error()));
		return;
	}
	activeTransfer->handle = *started;
}

#define FILE_RECEIVE_DLG_LIMIT 4

class CFileReceiveDialog final : public CFileDialog
{
public:
	CFileReceiveDialog() : CFileDialog(FALSE) {}

protected:
	BOOL OnFileNameOK() override
	{
		const CString pathName = GetPathName();
		if (GetFileAttributes(pathName) == INVALID_FILE_ATTRIBUTES)
			return FALSE;
		const HANDLE file = ::CreateFile(pathName, GENERIC_WRITE, 0, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) {
			AfxMessageBox(IDS_FILERCV_SHARE, MB_OK | MB_ICONEXCLAMATION);
			return TRUE;
		}
		CloseHandle(file);
		return FALSE;
	}
};

void ChatReceiveFile(CUserInfo* user, char* message)
{
	static int dialogCount = 0;
	if (!user || !theApp.m_bAllowFileTX || !bCanViewUnrated())
		return;
	if (!FileTransferCapacityAvailable()) {
		ShowTransferCapacityMessage();
		return;
	}
	auto offer = ParseDccOffer(message);
	if (!offer)
		return;
	const auto expectedPeerAddress = NumericPeerAddress(user);
	if (expectedPeerAddress && *expectedPeerAddress != offer->peerAddress) {
		CString warning("Rejected a DCC file offer whose address did not match ");
		warning += user->GetScreenName();
		warning += ".";
		AfxMessageBox(warning, MB_OK | MB_ICONEXCLAMATION);
		return;
	}
	if (!expectedPeerAddress) {
		CString warning("The sender's IRC identity does not expose a numeric address. Connect to ");
		warning += offer->peerAddress.c_str();
		warning += " for this DCC file transfer?";
		if (AfxMessageBox(warning, MB_YESNO | MB_ICONEXCLAMATION) != IDYES)
			return;
	}

	CString sizeText;
	sizeText.LoadString(IDS_FILESIZE_FORMAT);
	VERIFY(ReplaceToken(sizeText, CString("%1"), UInt64Text(offer->fileSize)));
	CString caption;
	caption.LoadString(IDS_ACCEPT_FILE_MESG);
	VERIFY(ReplaceToken(caption, CString("%1"), user->GetScreenName()));
	VERIFY(ReplaceToken(caption, CString("%2"), CString(offer->fileName.c_str())));
	VERIFY(ReplaceToken(caption, CString("%3"), sizeText));

	if (dialogCount >= FILE_RECEIVE_DLG_LIMIT)
		return;
	++dialogCount;
	const int accepted = AfxMessageBox(caption, MB_YESNO);
	if (accepted != IDYES) {
		--dialogCount;
		return;
	}

	CFileReceiveDialog dialog;
	dialog.m_ofn.Flags |= OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
	CString title;
	title.LoadString(IDS_TITLE_FILEDLG_RCV);
	VERIFY(ReplaceToken(title, CString("%1"), user->GetScreenName()));
	dialog.m_ofn.lpstrTitle = title;
	TryCopyBuffer(dialog.m_ofn.lpstrFile, dialog.m_ofn.nMaxFile, offer->fileName.c_str());
	dialog.m_ofn.lpstrInitialDir = theApp.m_strFileTXDir;
	char oldDirectory[MAX_PATH]{};
	char newDirectory[MAX_PATH]{};
	GetCurrentDirectory(MAX_PATH, oldDirectory);
	char filter[50]{};
	VERIFY(FillInFilter(dialog, filter, sizeof(filter)));
	int result = theApp.DoModalDlg(&dialog);
	if (result == IDCANCEL && CommDlgExtendedError() == FNERR_INVALIDFILENAME) {
		dialog.m_ofn.lpstrFile[0] = '\0';
		result = theApp.DoModalDlg(&dialog);
	}
	if (result != IDOK) {
		--dialogCount;
		return;
	}

	auto transfer = std::make_unique<FILETXINFO>();
	const CString pathName = dialog.GetPathName();
	if (!TryCopyArray(transfer->pathName, static_cast<LPCTSTR>(pathName))) {
		--dialogCount;
		return;
	}
	transfer->fileSize = offer->fileSize;

	auto* progress = new CFileProgress;
	if (!progress) {
		--dialogCount;
		return;
	}
	transfer->progDlg = progress;
	transfer->sending = FALSE;
	transfer->active = TRUE;
	progress->m_fileTX = transfer.get();
	progress->m_bytesTotal = UInt64Text(transfer->fileSize);
	progress->m_iBytesTotal = transfer->fileSize;
	progress->m_strOtherGuy = user->GetScreenName();
	progress->m_strFileName = GetFileNameFromFileDialog(dialog);
	progress->m_strStatus.LoadString(IDS_FILE_CONNECTING);
	progress->m_strXferredLabel.LoadString(IDS_BYTES_RECEIVED);
	progress->m_bSending = FALSE;
	progress->m_transferPhase = FileTransferPhase::connecting;
	VERIFY(ReplaceToken(progress->m_strStatus, CString("%1"), progress->m_strOtherGuy));
	if (!progress->Create(IDD_FILE_TRANSFER)) {
		progress->m_fileTX = NULL;
		delete progress;
		--dialogCount;
		return;
	}
	progress->UpdateProgress(0);
	if (!AddToFileProgressStore(progress)) {
		progress->DestroyWindow();
		progress->m_fileTX = NULL;
		delete progress;
		ShowTransferCapacityMessage();
		--dialogCount;
		return;
	}
	progress->ShowWindow(SW_SHOWNORMAL);
	FILETXINFO* activeTransfer = transfer.release();
	InstallWakeup(activeTransfer);

	comic_chat::net::DccConnectOptions options;
	options.peer_address = offer->peerAddress;
	options.port = offer->port;
	options.file_size = offer->fileSize;
	options.limits.receive_chunk_bytes = kFileChunkBytes;
	auto started = activeTransfer->engine.start_connect(std::move(options));
	if (!started)
		progress->FinishTransfer(FALSE, TransferErrorCategory(started.error()));
	else
		activeTransfer->handle = *started;
	if (GetCurrentDirectory(sizeof(newDirectory), newDirectory))
		theApp.m_strFileTXDir = newDirectory;
	SetCurrentDirectory(oldDirectory);
	--dialogCount;
}

#define MAXPOS 100

void CFileProgress::UpdateProgress(std::uint64_t bytes)
{
	m_iBytesTransferred = bytes;
	m_bytesSent = UInt64Text(bytes);
	if (GetDlgItem(IDC_BYTES_SENT))
		GetDlgItem(IDC_BYTES_SENT)->SetWindowText(m_bytesSent);
	if (!m_iBytesTotal)
		return;
	const double percent = std::min(1.0, static_cast<double>(bytes) / static_cast<double>(m_iBytesTotal));
	auto* control = static_cast<CProgressCtrl*>(GetDlgItem(IDC_FILEPROGRESS));
	if (control)
		control->SetPos(static_cast<int>(percent * MAXPOS));
	CString title;
	title.LoadString(m_bSending ? IDS_FILESEND_TITLE : IDS_FILEGET_TITLE);
	VERIFY(ReplaceToken(title, CString("%1"), UInt64Text(static_cast<std::uint64_t>(percent * 100.0))));
	VERIFY(ReplaceToken(title, CString("%2"), m_strFileName));
	VERIFY(ReplaceToken(title, CString("%3"), m_strOtherGuy));
	SetWindowText(title);
}

FileTransferSnapshot CFileProgress::GetTransferSnapshot() const
{
	ASSERT(AfxGetThread() == AfxGetApp());
	const auto handle = m_fileTX ? m_fileTX->handle : comic_chat::net::DccTransferHandle{};
	return FileTransferSnapshot{
		handle.generation,
		handle.transfer,
		m_bSending ? FileTransferDirection::send : FileTransferDirection::receive,
		static_cast<LPCTSTR>(m_strOtherGuy),
		static_cast<LPCTSTR>(m_strFileName),
		m_iBytesTransferred,
		m_iBytesTotal,
		m_transferPhase,
		m_transferError,
		m_fileTX && m_fileTX->active != FALSE,
	};
}

void CFileProgress::FinishTransfer(BOOL completed, FileTransferError error)
{
	if (!m_fileTX)
		return;
	m_fileTX->engine.set_wakeup({});
	m_fileTX->engine.stop();
	CloseTransferFile(m_fileTX);
	m_fileTX->active = FALSE;
	m_transferError = completed ? FileTransferError::none : error;
	m_transferPhase = completed
		? FileTransferPhase::completed
		: (error == FileTransferError::timed_out ? FileTransferPhase::timed_out : FileTransferPhase::failed);
	CString status;
	if (completed)
		status.LoadString(m_bSending ? IDS_FILE_SENT : IDS_FILE_RECEIVED);
	else
		status.LoadString(error == FileTransferError::timed_out ? IDS_FILETIMEOUT : IDS_CONNECTION_FAILED);
	if (GetDlgItem(IDC_CX_STATUS))
		GetDlgItem(IDC_CX_STATUS)->SetWindowText(status);
}

void CFileProgress::PumpTransferEvents()
{
	if (!m_fileTX || !m_fileTX->active)
		return;
	if (m_pumpingTransferEvents) {
		m_transferWakePending = true;
		return;
	}
	m_pumpingTransferEvents = true;
	m_transferWakePending = false;
	for (std::size_t batch = 0; batch < kMaximumEventBatchesPerWake; ++batch) {
		auto events = m_fileTX->engine.poll_events(kMaximumEventBatch);
		if (events.empty())
			break;
		for (auto& event : events) {
			if (!m_fileTX->active || event.handle != m_fileTX->handle)
				continue;
			std::visit([this](auto&& body) {
				using Body = std::remove_cvref_t<decltype(body)>;
				if constexpr (std::is_same_v<Body, comic_chat::net::DccListening>) {
					if (!m_fileTX->sending || m_fileTX->offerSent)
						return;
					auto* protocol = GetIrcProto();
					if (!protocol || !TryFormatOutBuff("%.*s SEND %s %lu %u %llu%c",
							static_cast<int>(sizeof(fileDCCID)), fileDCCID,
							m_fileTX->quotedFileName.c_str(),
							static_cast<unsigned long>(body.legacy_ipv4_decimal),
							static_cast<unsigned>(body.port),
							static_cast<unsigned long long>(m_fileTX->fileSize), 0x01) ||
						!protocol->bChatSendPrivMesg(m_fileTX->recipient.c_str(), NULL, GetOutBuff())) {
						FinishTransfer(FALSE, FileTransferError::protocol);
						return;
					}
					m_fileTX->offerSent = TRUE;
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccPeerOffered>) {
					bool accept = m_fileTX->offerSent;
					if (accept && m_fileTX->expectedPeerAddress)
						accept = *m_fileTX->expectedPeerAddress == body.peer_address;
					else if (accept) {
						CString prompt("Accept the DCC connection from ");
						prompt += body.peer_address.c_str();
						prompt += "?";
						accept = AfxMessageBox(prompt, MB_YESNO | MB_ICONQUESTION) == IDYES;
					}
					auto posted = accept
						? m_fileTX->engine.post(comic_chat::net::DccAcceptPeer{m_fileTX->handle, body.peer})
						: m_fileTX->engine.post(comic_chat::net::DccRejectPeer{m_fileTX->handle, body.peer});
					if (!posted)
						FinishTransfer(FALSE, TransferErrorCategory(posted.error()));
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccPeerConnected>) {
					if (!m_fileTX->sending && m_fileTX->hFile == INVALID_HANDLE_VALUE) {
						m_fileTX->hFile = ::CreateFile(m_fileTX->pathName, GENERIC_WRITE, 0, NULL,
							CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
						if (m_fileTX->hFile == INVALID_HANDLE_VALUE) {
							CString saveError;
							saveError.LoadString(ID_ERR_SAVE);
							VERIFY(ReplaceToken(saveError, CString("%1"), m_fileTX->pathName));
							AfxMessageBox(saveError);
							FinishTransfer(FALSE, FileTransferError::file_io);
							return;
						}
					}
					m_transferPhase = FileTransferPhase::transferring;
					CString status;
					status.LoadString(IDS_FILE_CONNECT);
					GetDlgItem(IDC_CX_STATUS)->SetWindowText(status);
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccWritableCredit>) {
					if (!m_fileTX->sending)
						return;
					std::size_t credit = body.bytes;
					while (credit && m_fileTX->fileOffset < m_fileTX->fileSize) {
						const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
							std::min(credit, kFileChunkBytes), m_fileTX->fileSize - m_fileTX->fileOffset));
						std::vector<std::byte> bytes(amount);
						DWORD read = 0;
						if (!ReadFile(m_fileTX->hFile, bytes.data(), static_cast<DWORD>(bytes.size()), &read, NULL) ||
							read == 0) {
							FinishTransfer(FALSE, FileTransferError::file_io);
							return;
						}
						bytes.resize(static_cast<std::size_t>(read));
						const bool final = m_fileTX->fileOffset + read == m_fileTX->fileSize;
						auto queued = m_fileTX->engine.post(comic_chat::net::DccQueueChunk{
							m_fileTX->handle, std::move(bytes), final});
						if (!queued) {
							FinishTransfer(FALSE, TransferErrorCategory(queued.error()));
							return;
						}
						m_fileTX->fileOffset += read;
						credit -= read;
					}
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccChunkReceived>) {
					if (m_fileTX->sending || !body.bytes || m_fileTX->fileOffset > m_fileTX->fileSize ||
						body.offset != m_fileTX->fileOffset ||
						body.bytes->size() > m_fileTX->fileSize - m_fileTX->fileOffset) {
						FinishTransfer(FALSE, FileTransferError::protocol);
						return;
					}
					if (!WriteAll(m_fileTX->hFile, *body.bytes)) {
						FinishTransfer(FALSE, FileTransferError::file_io);
						return;
					}
					m_fileTX->fileOffset += body.bytes->size();
					auto committed = m_fileTX->engine.post(comic_chat::net::DccCommitReceived{
						m_fileTX->handle, m_fileTX->fileOffset});
					if (!committed) {
						FinishTransfer(FALSE, TransferErrorCategory(committed.error()));
						return;
					}
					UpdateProgress(m_fileTX->fileOffset);
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccProgress>) {
					if (m_fileTX->sending)
						UpdateProgress(body.peer_committed);
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccCompleted>) {
					UpdateProgress(body.bytes);
					FinishTransfer(TRUE);
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccClosed>) {
					const bool timeout = body.reason.find("timeout") != std::string::npos;
					FinishTransfer(FALSE, timeout ? FileTransferError::timed_out : FileTransferError::connection);
				} else if constexpr (std::is_same_v<Body, comic_chat::net::DccDiagnostic>) {
					TRACE("DCC diagnostic [%s].\n", body.code.c_str());
					if (body.code == "accept-timeout" || body.code == "connect-timeout" ||
						body.code == "idle-timeout")
						FinishTransfer(FALSE, FileTransferError::timed_out);
				}
			}, event.body);
		}
		if (events.size() < kMaximumEventBatch)
			break;
	}
	m_pumpingTransferEvents = false;
	if (m_transferWakePending && m_fileTX && m_fileTX->active)
		PostMessage(WM_STATCHANGE, WP_NETWORK_EVENT, 0);
}

LRESULT CFileProgress::OnStatChange(WPARAM wParam, LPARAM lParam)
{
	(void)lParam;
	CString status;
	switch (wParam) {
	case WP_CONNECTED:
		status.LoadString(IDS_FILE_CONNECT);
		GetDlgItem(IDC_CX_STATUS)->SetWindowText(status);
		break;
	case WP_FILESENT:
		FinishTransfer(TRUE);
		break;
	case WP_CONNECTFAILED:
		FinishTransfer(FALSE);
		break;
	case WP_TIMEOUT:
		FinishTransfer(FALSE, FileTransferError::timed_out);
		break;
	case WP_BYTESSENT:
		UpdateProgress(0);
		break;
	case WP_NETWORK_EVENT:
		PumpTransferEvents();
		break;
	default:
		break;
	}
	return 0;
}

BOOL CFileProgress::OnInitDialog()
{
	const BOOL initialized = CDialog::OnInitDialog();
	CMenu* menu = GetSystemMenu(FALSE);
	if (menu) {
		menu->DeleteMenu(SC_SIZE, MF_BYCOMMAND);
		menu->DeleteMenu(SC_MAXIMIZE, MF_BYCOMMAND);
	}
	return initialized;
}

void CFileProgress::OnCancel()
{
	if (m_fileTX && m_fileTX->active) {
		(void)m_fileTX->engine.post(comic_chat::net::DccCancel{m_fileTX->handle, "user cancelled"});
		m_fileTX->engine.set_wakeup({});
		m_fileTX->engine.stop();
		CloseTransferFile(m_fileTX);
		m_fileTX->active = FALSE;
		m_transferPhase = FileTransferPhase::cancelled;
		m_transferError = FileTransferError::cancelled;
	}
	CDialog::OnCancel();
}
