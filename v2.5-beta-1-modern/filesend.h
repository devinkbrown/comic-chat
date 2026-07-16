// filesend.h : header file
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/////////////////////////////////////////////////////////////////////////////

// CFileProgress dialog

struct FILETXINFO;
class CRoomInfo;
class CUserInfo;

enum class FileTransferPhase : std::uint8_t {
	awaiting_peer,
	connecting,
	transferring,
	completed,
	failed,
	timed_out,
	cancelled,
};

enum class FileTransferDirection : std::uint8_t {
	send,
	receive,
};

enum class FileTransferError : std::uint8_t {
	none,
	address_validation,
	connection,
	protocol,
	file_io,
	resource_limit,
	timed_out,
	cancelled,
};

struct FileTransferSnapshot final {
	const std::uint64_t generation{};
	const std::uint64_t transfer_id{};
	const FileTransferDirection direction{FileTransferDirection::receive};
	const std::string peer_display_name;
	const std::string sanitized_basename;
	const std::uint64_t bytes_done{};
	const std::uint64_t total{};
	const FileTransferPhase state{FileTransferPhase::connecting};
	const FileTransferError error{FileTransferError::none};
	const bool cancellable{};
};

class CFileProgress : public CDialog
{
// Construction
public:
	CFileProgress(CWnd* pParent = NULL);   // standard constructor
	~CFileProgress();

// Dialog Data
	//{{AFX_DATA(CFileProgress)
	enum { IDD = IDD_FILE_TRANSFER };
	CProgressCtrl	m_fileProgress;
	CString	m_bytesSent;
	CString	m_bytesTotal;
	CString	m_strStatus;
	CString	m_strXferredLabel;
	//}}AFX_DATA
	std::uint64_t m_iBytesTotal;
	CString m_strFileName;
	CString m_strOtherGuy;
	BOOL m_bSending;
	FILETXINFO *m_fileTX;
	FileTransferSnapshot GetTransferSnapshot() const;


// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CFileProgress)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:

	// Generated message map functions
	//{{AFX_MSG(CFileProgress)
	virtual void OnCancel();
	virtual BOOL OnInitDialog();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
	afx_msg LRESULT OnStatChange(WPARAM wParam, LPARAM lParam);
	friend class CRoomInfo;
	friend void ChatReceiveFile(CUserInfo* user, char* message);
	void PumpTransferEvents();
	void UpdateProgress(std::uint64_t bytes);
	void FinishTransfer(BOOL completed, FileTransferError error = FileTransferError::connection);
	FileTransferPhase m_transferPhase;
	FileTransferError m_transferError;
	std::uint64_t m_iBytesTransferred;
	bool m_pumpingTransferEvents;
	bool m_transferWakePending;
};

// UI-thread snapshot feed for modern status rows and icon indicators. Values
// deliberately omit local paths, IRC credentials, and engine internals.
std::vector<FileTransferSnapshot> GetFileTransferSnapshots();
