//#define _AFXDLL // Required if you change project settings to shared library later
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Target Windows 10
#endif

#include <afxwin.h>
#include <afxcmn.h>
#include <afxstr.h>
#include <atlstr.h>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <cstdint>
#include "sqlite3.h"
// Include LibSQLLite header to access ORM types. Link against LibSQLLite.lib.
#include "../LibSQLLite/LibSQLLite.h"

// Control IDs
#define IDC_BTN_REFRESH   1001
#define IDC_BTN_INSERT    1002
#define IDC_GRID          1003
#define IDC_BTN_UPDATE    1004
#define IDC_BTN_DELETE    1005
#define IDC_BTN_CLEAR     1006
#define IDC_EDIT_NAME     1010
#define IDC_EDIT_ENGINE   1011
#define IDC_STATIC_NAME   1012
#define IDC_STATIC_ENGINE 1013
#define IDC_STATIC_STATUS 1014


// ============================================================================
// --- TABLE MODEL 1: Profiles (Demostrating INTEGER, REAL, TEXT, BLOB) ---
// ============================================================================

struct Profile
{
    int64_t id = -1;             // INTEGER PRIMARY KEY
    CString name;                // TEXT
    CString engine;              // TEXT
    double rating = 0.0;         // REAL
    std::vector<uint8_t> avatar; // BLOB
};

inline TableSchema<Profile> MakeProfileSchema()
{
    TableSchema<Profile> schema;
    schema.tableName = "Profiles";
    schema.primaryKeyName = "ID";
    schema.getPrimaryKey = [](const Profile& p) { return p.id; };
    schema.setPrimaryKey = [](Profile& p, int64_t id) { p.id = id; };

    // TEXT Datatype
    schema.columns.push_back(ColumnMapping<Profile>{
        "Name", ColumnType::Text,
            [](const Profile& p, sqlite3_stmt* stmt, int idx) {
            CStringA narrow(p.name);
            sqlite3_bind_text(stmt, idx, narrow.GetString(), -1, SQLITE_TRANSIENT);
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx) {
            const unsigned char* txt = sqlite3_column_text(stmt, idx);
            p.name = CString(txt ? reinterpret_cast<const char*>(txt) : "");
        }
    });

    // TEXT Datatype
    schema.columns.push_back(ColumnMapping<Profile>{
        "Engine", ColumnType::Text,
            [](const Profile& p, sqlite3_stmt* stmt, int idx) {
            CStringA narrow(p.engine);
            sqlite3_bind_text(stmt, idx, narrow.GetString(), -1, SQLITE_TRANSIENT);
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx) {
            const unsigned char* txt = sqlite3_column_text(stmt, idx);
            p.engine = CString(txt ? reinterpret_cast<const char*>(txt) : "");
        }
    });

    // REAL Datatype (IEEE 8-Byte Floating-Point)
    schema.columns.push_back(ColumnMapping<Profile>{
        "Rating", ColumnType::Real,
            [](const Profile& p, sqlite3_stmt* stmt, int idx) {
            sqlite3_bind_double(stmt, idx, p.rating);
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx) {
            p.rating = sqlite3_column_double(stmt, idx);
        }
    });

    // BLOB Datatype (Raw Binary Data)
    schema.columns.push_back(ColumnMapping<Profile>{
        "Avatar", ColumnType::Blob,
            [](const Profile& p, sqlite3_stmt* stmt, int idx) {
            if (!p.avatar.empty())
            {
                sqlite3_bind_blob(stmt, idx, p.avatar.data(), static_cast<int>(p.avatar.size()), SQLITE_TRANSIENT);
            }
            else
            {
                sqlite3_bind_null(stmt, idx); // NULL support
            }
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx) {
            const void* blobData = sqlite3_column_blob(stmt, idx);
            int blobBytes = sqlite3_column_bytes(stmt, idx);
            if (blobData && blobBytes > 0)
            {
                const uint8_t* bytePtr = static_cast<const uint8_t*>(blobData);
                p.avatar.assign(bytePtr, bytePtr + blobBytes);
            }
            else
            {
                p.avatar.clear();
            }
        }
    });

    return schema;
}

// ============================================================================
// --- TABLE MODEL 2: Audit Logs (Demonstrating INTEGER Datatypes) ---
// ============================================================================

struct AuditLog
{
    int64_t logId = -1;  // INTEGER PRIMARY KEY
    CString action;      // TEXT
    int64_t profileId = 0;// INTEGER (Foreign key mapping)
};

inline TableSchema<AuditLog> MakeAuditLogSchema()
{
    TableSchema<AuditLog> schema;
    schema.tableName = "AuditLogs";
    schema.primaryKeyName = "LogID";
    schema.getPrimaryKey = [](const AuditLog& a) { return a.logId; };
    schema.setPrimaryKey = [](AuditLog& a, int64_t id) { a.logId = id; };

    schema.columns.push_back(ColumnMapping<AuditLog>{
        "Action", ColumnType::Text,
            [](const AuditLog& a, sqlite3_stmt* stmt, int idx) {
            CStringA narrow(a.action);
            sqlite3_bind_text(stmt, idx, narrow.GetString(), -1, SQLITE_TRANSIENT);
        },
            [](AuditLog& a, sqlite3_stmt* stmt, int idx) {
            const unsigned char* txt = sqlite3_column_text(stmt, idx);
            a.action = CString(txt ? reinterpret_cast<const char*>(txt) : "");
        }
    });

    schema.columns.push_back(ColumnMapping<AuditLog>{
        "ProfileID", ColumnType::Integer,
            [](const AuditLog& a, sqlite3_stmt* stmt, int idx) {
            sqlite3_bind_int64(stmt, idx, a.profileId);
        },
            [](AuditLog& a, sqlite3_stmt* stmt, int idx) {
            a.profileId = sqlite3_column_int64(stmt, idx);
        }
    });

    return schema;
}

// ============================================================================
// --- MAIN DIALOG WITH EXTENDED DATATYPE SUPPORT ---
// ============================================================================

class CMainGridDialog : public CDialog
{
public:
    CMainGridDialog()
        : CDialog(),
        m_ProfileRepo(MakeProfileSchema()),
        m_AuditRepo(MakeAuditLogSchema())
    {
        ZeroMemory(&m_DlgTemplate, sizeof(m_DlgTemplate));
        m_DlgTemplate.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
        m_DlgTemplate.dlg.cx = 320;
        m_DlgTemplate.dlg.cy = 220;

        InitModalIndirect(&m_DlgTemplate.dlg, nullptr);
    }

protected:
    struct
    {
        DLGTEMPLATE dlg;
        WORD menu = 0;
        WORD windowClass = 0;
        WORD title = 0;
    } m_DlgTemplate;

    CListCtrl m_GridCtrl;
    CButton   m_BtnRefresh, m_BtnInsert, m_BtnUpdate, m_BtnDelete, m_BtnClear;
    CEdit     m_EditName, m_EditEngine;
    CStatic   m_StaticName, m_StaticEngine, m_StaticStatus;

    DatabaseContext           m_DbContext;
    SqliteRepository<Profile>  m_ProfileRepo;
    SqliteRepository<AuditLog> m_AuditRepo;

    int64_t m_SelectedId = -1;

    virtual BOOL OnInitDialog() override
    {
        CDialog::OnInitDialog();

        SetWindowText(_T("MFC Generic SQLite ORM (Supports ALL SQLite Datatypes)"));
        SetWindowPos(NULL, 0, 0, 650, 500, SWP_NOMOVE | SWP_NOZORDER);

        // UI Initialization
        m_StaticName.Create(_T("Name:"), WS_CHILD | WS_VISIBLE, CRect(20, 18, 65, 34), this, IDC_STATIC_NAME);
        m_EditName.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(70, 15, 290, 36), this, IDC_EDIT_NAME);
        m_StaticEngine.Create(_T("Engine:"), WS_CHILD | WS_VISIBLE, CRect(300, 18, 350, 34), this, IDC_STATIC_ENGINE);
        m_EditEngine.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, CRect(355, 15, 610, 36), this, IDC_EDIT_ENGINE);

        m_BtnInsert.Create(_T("Insert"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(20, 50, 130, 82), this, IDC_BTN_INSERT);
        m_BtnUpdate.Create(_T("Update Selected"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(140, 50, 270, 82), this, IDC_BTN_UPDATE);
        m_BtnDelete.Create(_T("Delete Selected"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(280, 50, 410, 82), this, IDC_BTN_DELETE);
        m_BtnClear.Create(_T("Clear Fields"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(420, 50, 530, 82), this, IDC_BTN_CLEAR);
        m_BtnRefresh.Create(_T("Refresh Grid"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, CRect(540, 50, 610, 82), this, IDC_BTN_REFRESH);

        m_GridCtrl.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT, CRect(20, 92, 610, 400), this, IDC_GRID);
        m_GridCtrl.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        m_GridCtrl.InsertColumn(0, _T("ID (INT64)"), LVCFMT_LEFT, 80);
        m_GridCtrl.InsertColumn(1, _T("Name (TEXT)"), LVCFMT_LEFT, 180);
        m_GridCtrl.InsertColumn(2, _T("Engine (TEXT)"), LVCFMT_LEFT, 150);
        m_GridCtrl.InsertColumn(3, _T("Rating (REAL)"), LVCFMT_LEFT, 90);
        m_GridCtrl.InsertColumn(4, _T("Avatar Size (BLOB)"), LVCFMT_LEFT, 110);

        m_StaticStatus.Create(_T("Ready."), WS_CHILD | WS_VISIBLE | SS_LEFT, CRect(20, 410, 610, 430), this, IDC_STATIC_STATUS);

        // Compute Database Path
        TCHAR modulePath[MAX_PATH] = {};
        GetModuleFileName(nullptr, modulePath, MAX_PATH);
        CString dbPath(modulePath);
        const int lastSlash = dbPath.ReverseFind(_T('\\'));
        if (lastSlash >= 0) dbPath = dbPath.Left(lastSlash + 1);
        dbPath += _T("mfc_app_multitable.db");

        // 1. Register Repositories
        m_DbContext.RegisterRepository<Profile>(m_ProfileRepo);
        m_DbContext.RegisterRepository<AuditLog>(m_AuditRepo);

        // 2. Open DB Connection & Install Schemas
        CString errorMsg;
        if (!m_DbContext.Open(dbPath, errorMsg))
        {
            AfxMessageBox(errorMsg);
        }

        RefreshGridData();
        UpdateButtonStates();

        return TRUE;
    }

    void RefreshGridData()
    {
        m_GridCtrl.DeleteAllItems();

        std::vector<Profile> profiles;
        CString errorMsg;

        if (m_ProfileRepo.GetAll(profiles, errorMsg))
        {
            for (size_t i = 0; i < profiles.size(); ++i)
            {
                CString strId, strRating, strBlobSize;
                strId.Format(_T("%lld"), profiles[i].id);
                strRating.Format(_T("%.2f"), profiles[i].rating);
                strBlobSize.Format(_T("%Iu bytes"), profiles[i].avatar.size());

                int insertedPos = m_GridCtrl.InsertItem(static_cast<int>(i), strId);
                m_GridCtrl.SetItemText(insertedPos, 1, profiles[i].name);
                m_GridCtrl.SetItemText(insertedPos, 2, profiles[i].engine);
                m_GridCtrl.SetItemText(insertedPos, 3, strRating);
                m_GridCtrl.SetItemText(insertedPos, 4, strBlobSize);
            }
        }
        else
        {
            SetStatus(errorMsg);
        }

        m_SelectedId = -1;
        UpdateButtonStates();
    }

    void InsertProfile()
    {
        Profile prof;
        m_EditName.GetWindowText(prof.name);
        m_EditEngine.GetWindowText(prof.engine);
        prof.name.Trim();
        prof.engine.Trim();

        if (prof.name.IsEmpty())
        {
            AfxMessageBox(_T("Please enter a Name before inserting."));
            m_EditName.SetFocus();
            return;
        }

        // Demo data insertion for REAL & BLOB types
        prof.rating = 4.85; // Demo real float value
        prof.avatar = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }; // Mock binary payload header

        CString errorMsg;
        if (!m_ProfileRepo.Insert(prof, errorMsg))
        {
            AfxMessageBox(errorMsg);
            return;
        }

        // Writes to Second Table (Audit Log)
        AuditLog log;
        log.action = _T("INSERT");
        log.profileId = prof.id;
        m_AuditRepo.Insert(log, errorMsg);

        SetStatus(_T("Record (including Real & Blob fields) inserted and logged to AuditLogs."));
        ClearInputFields();
        RefreshGridData();
    }

    void UpdateProfile()
    {
        if (m_SelectedId < 0) return;

        Profile prof;
        prof.id = m_SelectedId;
        m_EditName.GetWindowText(prof.name);
        m_EditEngine.GetWindowText(prof.engine);
        prof.name.Trim();
        prof.engine.Trim();

        if (prof.name.IsEmpty()) return;

        // Update real and blob types as well
        prof.rating = 5.00;
        prof.avatar = { 0xFF, 0xD8, 0xFF, 0xE0 }; // Simulated jpeg header update

        CString errorMsg;
        if (!m_ProfileRepo.Update(prof, errorMsg))
        {
            AfxMessageBox(errorMsg);
            return;
        }

        // Writes to Second Table (Audit Log)
        AuditLog log;
        log.action = _T("UPDATE");
        log.profileId = prof.id;
        m_AuditRepo.Insert(log, errorMsg);

        CString status;
        status.Format(_T("Record %lld updated."), m_SelectedId);
        SetStatus(status);
        ClearInputFields();
        RefreshGridData();
    }

    void DeleteProfile()
    {
        if (m_SelectedId < 0) return;

        CString confirmMsg;
        confirmMsg.Format(_T("Delete record %lld?"), m_SelectedId);
        if (AfxMessageBox(confirmMsg, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;

        CString errorMsg;
        if (!m_ProfileRepo.Delete(m_SelectedId, errorMsg))
        {
            AfxMessageBox(errorMsg);
            return;
        }

        // Writes to Second Table (Audit Log)
        AuditLog log;
        log.action = _T("DELETE");
        log.profileId = m_SelectedId;
        m_AuditRepo.Insert(log, errorMsg);

        CString status;
        status.Format(_T("Record %lld deleted."), m_SelectedId);
        SetStatus(status);
        ClearInputFields();
        RefreshGridData();
    }

    void ClearInputFields()
    {
        m_EditName.SetWindowText(_T(""));
        m_EditEngine.SetWindowText(_T(""));
        m_SelectedId = -1;
        m_GridCtrl.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        UpdateButtonStates();
    }

    void SetStatus(const CString& text)
    {
        if (m_StaticStatus.GetSafeHwnd())
            m_StaticStatus.SetWindowText(text);
    }

    void UpdateButtonStates()
    {
        const BOOL hasSelection = (m_SelectedId >= 0);
        if (m_BtnUpdate.GetSafeHwnd()) m_BtnUpdate.EnableWindow(hasSelection);
        if (m_BtnDelete.GetSafeHwnd()) m_BtnDelete.EnableWindow(hasSelection);
    }

    void OnGridSelectionChanged(int iItem)
    {
        if (iItem < 0)
        {
            m_SelectedId = -1;
            UpdateButtonStates();
            return;
        }

        CString strId = m_GridCtrl.GetItemText(iItem, 0);
        CString strName = m_GridCtrl.GetItemText(iItem, 1);
        CString strEngine = m_GridCtrl.GetItemText(iItem, 2);

        m_SelectedId = _ttoi64(strId);
        m_EditName.SetWindowText(strName);
        m_EditEngine.SetWindowText(strEngine);

        CString status;
        status.Format(_T("Selected record %lld."), m_SelectedId);
        SetStatus(status);
        UpdateButtonStates();
    }

    virtual BOOL OnNotify(WPARAM wParam, LPARAM lParam, LRESULT* pResult) override
    {
        NMHDR* pHdr = reinterpret_cast<NMHDR*>(lParam);
        if (pHdr != nullptr && pHdr->idFrom == IDC_GRID && pHdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* pNMLV = reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((pNMLV->uNewState & LVIS_SELECTED) && !(pNMLV->uOldState & LVIS_SELECTED))
            {
                OnGridSelectionChanged(pNMLV->iItem);
            }
            else if (m_GridCtrl.GetSelectedCount() == 0)
            {
                OnGridSelectionChanged(-1);
            }
        }
        return CDialog::OnNotify(wParam, lParam, pResult);
    }

    virtual BOOL OnCmdMsg(UINT nID, int nCode, void* pExtra, AFX_CMDHANDLERINFO* pHandlerInfo) override
    {
        if (nCode == CN_COMMAND)
        {
            switch (nID)
            {
            case IDC_BTN_REFRESH:
                RefreshGridData();
                SetStatus(_T("Grid refreshed."));
                return TRUE;
            case IDC_BTN_INSERT:
                InsertProfile();
                return TRUE;
            case IDC_BTN_UPDATE:
                UpdateProfile();
                return TRUE;
            case IDC_BTN_DELETE:
                DeleteProfile();
                return TRUE;
            case IDC_BTN_CLEAR:
                ClearInputFields();
                SetStatus(_T("Ready."));
                return TRUE;
            default:
                break;
            }
        }
        return CDialog::OnCmdMsg(nID, nCode, pExtra, pHandlerInfo);
    }

    virtual void OnCancel() override
    {
        m_DbContext.Close();
        CDialog::OnCancel();
    }

    virtual void OnOK() override
    {
        m_DbContext.Close();
        CDialog::OnOK();
    }
};

// --- MFC Application Setup ---
class CMfcSqliteApp : public CWinApp
{
public:
    virtual BOOL InitInstance() override
    {
        CWinApp::InitInstance();
        InitCommonControls();

        CMainGridDialog dlg;
        m_pMainWnd = &dlg;
        dlg.DoModal();
        m_pMainWnd = nullptr;
        return FALSE;
    }
};

CMfcSqliteApp standardRunApp;
