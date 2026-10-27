//#define _AFXDLL // Required if you change project settings to shared library later
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Target Windows 10
#endif

#include <afxwin.h>
#include <afxcmn.h>
#include <afxdlgs.h>
#include <afxstr.h>
#include <atlstr.h>
#include <atlimage.h>
#include <vector>
#include <random>
#include <cmath>
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
#define IDC_STATIC_PREVIEW_LABEL 1015
#define IDC_PREVIEW       1016
#define IDC_EDIT_RATING   1017
#define IDC_STATIC_RATING 1018
#define IDC_BTN_UPLOAD    1019
#define IDC_BTN_RANDOM    1020

// Largest image file accepted by Upload Image (kept small so the demo database stays light).
const ULONGLONG kMaxUploadBytes = 10 * 1024 * 1024;

// ============================================================================
// --- AVATAR IMAGE HELPERS (PNG bytes <-> CImage) ---
// ============================================================================

// Draws a random abstract picture (background, shapes, and the name's initial) and returns it as PNG bytes.
inline std::vector<uint8_t> GenerateRandomAvatarPng(const CString& name, std::mt19937& rng)
{
    const int size = 160;
    std::uniform_int_distribution<int> channel(0, 255);
    std::uniform_int_distribution<int> coord(-size / 4, size);
    std::uniform_int_distribution<int> extent(size / 6, size / 2);
    std::uniform_int_distribution<int> shapeCount(4, 9);
    std::uniform_int_distribution<int> shapeKind(0, 2);
    auto randomColor = [&]()
    { return RGB(channel(rng), channel(rng), channel(rng)); };

    CImage image;
    if (!image.Create(size, size, 24))
    {
        return {};
    }

    HDC hdc = image.GetDC();
    CDC* pDC = CDC::FromHandle(hdc);
    pDC->FillSolidRect(0, 0, size, size, randomColor());

    const int count = shapeCount(rng);
    for (int i = 0; i < count; ++i)
    {
        CBrush brush(randomColor());
        CPen pen(PS_SOLID, 2, randomColor());
        CBrush* pOldBrush = pDC->SelectObject(&brush);
        CPen* pOldPen = pDC->SelectObject(&pen);

        const int x = coord(rng), y = coord(rng);
        const CRect rc(x, y, x + extent(rng), y + extent(rng));
        switch (shapeKind(rng))
        {

        case 0: pDC->Ellipse(rc);
            break;

        case 1: pDC->Rectangle(rc);
            break;

        default: pDC->RoundRect(rc, CPoint(rc.Width() / 3, rc.Height() / 3));
            break;
        }

        pDC->SelectObject(pOldPen);
        pDC->SelectObject(pOldBrush);
    }

    // The profile's initial on top, so each picture is recognisable in the preview.
    CString initial = name.IsEmpty() ? CString(_T("?")) : name.Left(1).MakeUpper();
    CFont font;
    font.CreatePointFont(720, _T("Segoe UI"), pDC);
    CFont* pOldFont = pDC->SelectObject(&font);
    pDC->SetBkMode(TRANSPARENT);
    CRect full(0, 0, size, size);
    pDC->SetTextColor(RGB(0, 0, 0));
    CRect shadow(full);
    shadow.OffsetRect(3, 3);
    pDC->DrawText(initial, shadow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    pDC->SetTextColor(RGB(255, 255, 255));
    pDC->DrawText(initial, full, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    pDC->SelectObject(pOldFont);
    image.ReleaseDC();

    // Encode to PNG in an in-memory stream, then copy the bytes out.
    std::vector<uint8_t> bytes;
    CComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(image.Save(stream, Gdiplus::ImageFormatPNG)))
    {
        return bytes;
    }

    STATSTG stat = {};
    HGLOBAL hGlobal = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream, &hGlobal)))
    {
        return bytes;
    }

    const void* pData = GlobalLock(hGlobal);
    if (pData)
    {
        const uint8_t* bytePtr = static_cast<const uint8_t*>(pData);
        bytes.assign(bytePtr, bytePtr + static_cast<size_t>(stat.cbSize.QuadPart));
        GlobalUnlock(hGlobal);
    }

    return bytes;
}

// Decodes image bytes (PNG, JPEG, BMP, GIF) into a CImage. Returns false for anything that isn't an image.
inline bool LoadImageFromBytes(const std::vector<uint8_t>& bytes, CImage& outImage)
{
    outImage.Destroy();
    if (bytes.empty())
    {
        return false;
    }

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!hGlobal)
    {
        return false;
    }

    void* pData = GlobalLock(hGlobal);
    if (!pData)
    {
        GlobalFree(hGlobal);
        return false;
    }

    memcpy(pData, bytes.data(), bytes.size());
    GlobalUnlock(hGlobal);

    CComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &stream))) // stream now owns hGlobal
    {
        GlobalFree(hGlobal);
        return false;
    }

    return SUCCEEDED(outImage.Load(stream));
}

// Static control that paints a CImage scaled to fit (keeping its aspect ratio), or a placeholder message.
class CImagePreview : public CStatic
{
public:
    void SetImageBytes(const std::vector<uint8_t>& bytes)
    {
        if (LoadImageFromBytes(bytes, m_Image))
        {
            m_Placeholder.Empty();
        }
        else
        {
            m_Placeholder = bytes.empty() ? _T("No image") : _T("Avatar BLOB is not an image");
        }

        Invalidate();
    }

    void Clear(const CString& placeholder = _T("Select a row to see its avatar"))
    {
        m_Image.Destroy();
        m_Placeholder = placeholder;
        Invalidate();
    }

protected:
    CImage  m_Image;
    CString m_Placeholder = _T("Select a row to see its avatar");

    afx_msg void OnPaint()
    {
        CPaintDC dc(this);
        CRect rc;
        GetClientRect(&rc);
        dc.FillSolidRect(rc, GetSysColor(COLOR_WINDOW));

        if (!m_Image.IsNull())
        {
            const double scale = (std::min)(static_cast<double>(rc.Width()) / m_Image.GetWidth(),
                static_cast<double>(rc.Height()) / m_Image.GetHeight());
            const int w = static_cast<int>(m_Image.GetWidth() * scale);
            const int h = static_cast<int>(m_Image.GetHeight() * scale);
            const CRect dest(rc.left + (rc.Width() - w) / 2, rc.top + (rc.Height() - h) / 2, 0, 0);
            dc.SetStretchBltMode(HALFTONE);
            m_Image.Draw(dc.GetSafeHdc(), dest.left, dest.top, w, h);
        }
        else
        {
            CFont* pFont = GetFont();
            CFont* pOldFont = pFont ? dc.SelectObject(pFont) : nullptr;
            dc.SetBkMode(TRANSPARENT);
            dc.SetTextColor(GetSysColor(COLOR_GRAYTEXT));
            dc.DrawText(m_Placeholder, rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (pOldFont)
            {
                dc.SelectObject(pOldFont);
            }
        }
    }

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CImagePreview, CStatic)
    ON_WM_PAINT()
END_MESSAGE_MAP()


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
    schema.columns.push_back(ColumnMapping<Profile>
    {
        "Name", ColumnType::Text,
            [](const Profile& p, sqlite3_stmt* stmt, int idx)
        {
            CStringA narrow(p.name);
            sqlite3_bind_text(stmt, idx, narrow.GetString(), -1, SQLITE_TRANSIENT);
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx)
        {
            const unsigned char* txt = sqlite3_column_text(stmt, idx);
            p.name = CString(txt ? reinterpret_cast<const char*>(txt) : "");
        }
    });

    // TEXT Datatype
    schema.columns.push_back(ColumnMapping<Profile>
    {
        "Engine", ColumnType::Text,
            [](const Profile& p, sqlite3_stmt* stmt, int idx)
        {
            CStringA narrow(p.engine);
            sqlite3_bind_text(stmt, idx, narrow.GetString(), -1, SQLITE_TRANSIENT);
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx)
        {
            const unsigned char* txt = sqlite3_column_text(stmt, idx);
            p.engine = CString(txt ? reinterpret_cast<const char*>(txt) : "");
        }
    });

    // REAL Datatype (IEEE 8-Byte Floating-Point)
    schema.columns.push_back(ColumnMapping<Profile>
    {
        "Rating", ColumnType::Real,
            [](const Profile& p, sqlite3_stmt* stmt, int idx)
        {
            sqlite3_bind_double(stmt, idx, p.rating);
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx)
        {
            p.rating = sqlite3_column_double(stmt, idx);
        }
    });

    // BLOB Datatype (Raw Binary Data)
    schema.columns.push_back(ColumnMapping<Profile>
    {
        "Avatar", ColumnType::Blob,
            [](const Profile& p, sqlite3_stmt* stmt, int idx)
        {
            if (!p.avatar.empty())
            {
                sqlite3_bind_blob(stmt, idx, p.avatar.data(), static_cast<int>(p.avatar.size()), SQLITE_TRANSIENT);
            }
            else
            {
                sqlite3_bind_null(stmt, idx); // NULL support
            }
        },
            [](Profile& p, sqlite3_stmt* stmt, int idx)
        {
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

    schema.columns.push_back(ColumnMapping<AuditLog>
    {
        "Action", ColumnType::Text,
            [](const AuditLog& a, sqlite3_stmt* stmt, int idx)
        {
            CStringA narrow(a.action);
            sqlite3_bind_text(stmt, idx, narrow.GetString(), -1, SQLITE_TRANSIENT);
        },
            [](AuditLog& a, sqlite3_stmt* stmt, int idx)
        {
            const unsigned char* txt = sqlite3_column_text(stmt, idx);
            a.action = CString(txt ? reinterpret_cast<const char*>(txt) : "");
        }
    });

    schema.columns.push_back(ColumnMapping<AuditLog>
    {
        "ProfileID", ColumnType::Integer,
            [](const AuditLog& a, sqlite3_stmt* stmt, int idx)
        {
            sqlite3_bind_int64(stmt, idx, a.profileId);
        },
            [](AuditLog& a, sqlite3_stmt* stmt, int idx)
        {
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
        : CDialog()
        , m_ProfileRepo(MakeProfileSchema())
        , m_AuditRepo(MakeAuditLogSchema())
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
    CButton   m_BtnRefresh, m_BtnInsert, m_BtnUpdate, m_BtnDelete, m_BtnClear, m_BtnUpload, m_BtnRandom;
    CEdit     m_EditName, m_EditEngine, m_EditRating;
    CStatic   m_StaticName, m_StaticEngine, m_StaticRating, m_StaticStatus, m_StaticPreviewLabel;
    CImagePreview m_Preview;
    CFont     m_UiFont;

    DatabaseContext             m_DbContext;
    SqliteRepository<Profile>   m_ProfileRepo;
    SqliteRepository<AuditLog>  m_AuditRepo;

    std::vector<Profile> m_Profiles; // Rows currently shown in the grid (item data = index)
    std::mt19937 m_Rng{ std::random_device{}() };
    std::vector<uint8_t> m_UploadedAvatar; // Image file chosen with Upload Image; empty = generate a random one

    int64_t m_SelectedId = -1;

    virtual BOOL OnInitDialog() override
    {
        CDialog::OnInitDialog();

        // The layout below is in 96-DPI pixels; S() scales it to the window's DPI.
        const int dpi = static_cast<int>(GetDpiForWindow(m_hWnd));
        auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
        auto R = [&S](int l, int t, int r, int b) { return CRect(S(l), S(t), S(r), S(b)); };

        SetWindowText(_T("MFC Generic SQLite ORM (Supports ALL SQLite Datatypes)"));
        SetWindowPos(NULL, 0, 0, S(860), S(500), SWP_NOMOVE | SWP_NOZORDER);

        // UI Initialization
        m_StaticName.Create(_T("Name:"), WS_CHILD | WS_VISIBLE, R(20, 18, 65, 34), this, IDC_STATIC_NAME);
        m_EditName.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, R(70, 15, 240, 36), this, IDC_EDIT_NAME);
        m_StaticEngine.Create(_T("Engine:"), WS_CHILD | WS_VISIBLE, R(250, 18, 300, 34), this, IDC_STATIC_ENGINE);
        m_EditEngine.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, R(305, 15, 455, 36), this, IDC_EDIT_ENGINE);
        m_StaticRating.Create(_T("Rating:"), WS_CHILD | WS_VISIBLE, R(465, 18, 515, 34), this, IDC_STATIC_RATING);
        m_EditRating.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_RIGHT, R(520, 15, 610, 36), this, IDC_EDIT_RATING);
        m_EditRating.SetCueBanner(_T("e.g. 4.5"));

        m_BtnInsert.Create(_T("Insert"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(20, 50, 120, 82), this, IDC_BTN_INSERT);
        m_BtnUpdate.Create(_T("Update Selected"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(130, 50, 250, 82), this, IDC_BTN_UPDATE);
        m_BtnDelete.Create(_T("Delete Selected"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(260, 50, 380, 82), this, IDC_BTN_DELETE);
        m_BtnClear.Create(_T("Clear Fields"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(390, 50, 495, 82), this, IDC_BTN_CLEAR);
        m_BtnRefresh.Create(_T("Refresh Grid"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(505, 50, 610, 82), this, IDC_BTN_REFRESH);

        m_GridCtrl.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT, R(20, 92, 610, 400), this, IDC_GRID);
        m_GridCtrl.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        // Column widths add up to the grid's inner width, so there is no horizontal scroll bar.
        m_GridCtrl.InsertColumn(0, _T("ID (INT64)"), LVCFMT_LEFT, S(70));
        m_GridCtrl.InsertColumn(1, _T("Name (TEXT)"), LVCFMT_LEFT, S(165));
        m_GridCtrl.InsertColumn(2, _T("Engine (TEXT)"), LVCFMT_LEFT, S(150));
        m_GridCtrl.InsertColumn(3, _T("Rating (REAL)"), LVCFMT_LEFT, S(90));
        m_GridCtrl.InsertColumn(4, _T("Avatar Size (BLOB)"), LVCFMT_LEFT, S(110));

        // Avatar preview: shows the selected row's BLOB decoded as an image.
        m_StaticPreviewLabel.Create(_T("Avatar (BLOB) preview:"), WS_CHILD | WS_VISIBLE, R(625, 18, 830, 34), this, IDC_STATIC_PREVIEW_LABEL);
        m_Preview.Create(_T(""), WS_CHILD | WS_VISIBLE | WS_BORDER, R(625, 40, 830, 245), this, IDC_PREVIEW);
        m_BtnUpload.Create(_T("Upload Image..."), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(625, 255, 725, 287), this, IDC_BTN_UPLOAD);
        m_BtnRandom.Create(_T("Use Random"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, R(730, 255, 830, 287), this, IDC_BTN_RANDOM);

        m_StaticStatus.Create(_T("Ready."), WS_CHILD | WS_VISIBLE | SS_LEFT, R(20, 410, 610, 430), this, IDC_STATIC_STATUS);

        // The in-memory dialog template has no DS_SETFONT, so without this every control would use the
        // bold System font, which doesn't fit the layout. Use the Windows UI font at this window's DPI.
        NONCLIENTMETRICS ncm = { sizeof(ncm) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi) &&
            m_UiFont.CreateFontIndirect(&ncm.lfMessageFont))
        {
            SendMessageToDescendants(WM_SETFONT, reinterpret_cast<WPARAM>(m_UiFont.GetSafeHandle()), TRUE);
        }

        // Compute Database Path
        TCHAR modulePath[MAX_PATH] = {};
        GetModuleFileName(nullptr, modulePath, MAX_PATH);
        CString dbPath(modulePath);
        const int lastSlash = dbPath.ReverseFind(_T('\\'));
        if (lastSlash >= 0)
        {
            dbPath = dbPath.Left(lastSlash + 1);
        }

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

        std::vector<Profile>& profiles = m_Profiles;
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
                m_GridCtrl.SetItemData(insertedPos, static_cast<DWORD_PTR>(i));
            }
        }
        else
        {
            SetStatus(errorMsg);
        }

        m_SelectedId = -1;
        ShowPreview(-1);
        UpdateButtonStates();
    }

    // Shows the uploaded image if there is one waiting to be saved, otherwise the given grid row's avatar.
    void ShowPreview(int iItem)
    {
        if (!m_UploadedAvatar.empty())
        {
            m_StaticPreviewLabel.SetWindowText(_T("Uploaded image (not saved yet):"));
            m_Preview.SetImageBytes(m_UploadedAvatar);
            return;
        }

        m_StaticPreviewLabel.SetWindowText(_T("Avatar (BLOB) preview:"));
        const size_t profileIdx = (iItem >= 0) ? static_cast<size_t>(m_GridCtrl.GetItemData(iItem)) : m_Profiles.size();
        if (profileIdx < m_Profiles.size())
        {
            m_Preview.SetImageBytes(m_Profiles[profileIdx].avatar);
        }
        else
        {
            m_Preview.Clear();
        }
    }

    // Lets the user pick an image file; it is stored as the Avatar BLOB on the next Insert or Update.
    void UploadImage()
    {
        CFileDialog dlg(TRUE, nullptr, nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
            _T("Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif)|*.png;*.jpg;*.jpeg;*.bmp;*.gif|All Files (*.*)|*.*||"), this);
        if (dlg.DoModal() != IDOK)
        {
            return;
        }

        std::vector<uint8_t> bytes;
        CFileException fileEx;
        CFile file;
        if (!file.Open(dlg.GetPathName(), CFile::modeRead | CFile::shareDenyWrite, &fileEx))
        {
            TCHAR reason[256] = {};
            fileEx.GetErrorMessage(reason, _countof(reason));
            AfxMessageBox(CString(_T("Could not open the file:\n")) + reason);
            return;
        }

        const ULONGLONG length = file.GetLength();
        if (length == 0 || length > kMaxUploadBytes)
        {
            AfxMessageBox(_T("Please choose an image file between 1 byte and 10 MB."));
            return;
        }

        bytes.resize(static_cast<size_t>(length));
        if (file.Read(bytes.data(), static_cast<UINT>(length)) != length)
        {
            AfxMessageBox(_T("Could not read the whole file."));
            return;
        }

        file.Close();

        // Only accept files that actually decode as an image.
        CImage probe;
        if (!LoadImageFromBytes(bytes, probe))
        {
            AfxMessageBox(_T("The selected file is not a supported image (PNG, JPEG, BMP or GIF)."));
            return;
        }

        m_UploadedAvatar = std::move(bytes);
        ShowPreview(-1);
        UpdateButtonStates();

        CString status;
        status.Format(_T("Image '%s' (%I64u bytes) will be saved on the next Insert or Update."),
            dlg.GetFileName().GetString(), length);
        SetStatus(status);
    }

    void DiscardUploadedImage()
    {
        m_UploadedAvatar.clear();
        ShowPreview(m_GridCtrl.GetNextItem(-1, LVNI_SELECTED));
        UpdateButtonStates();
    }

    // The uploaded image if one was chosen, otherwise a freshly generated random one.
    std::vector<uint8_t> AvatarForSave(const CString& name)
    {
        if (!m_UploadedAvatar.empty())
        {
            return m_UploadedAvatar;
        }

        return GenerateRandomAvatarPng(name, m_Rng);
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

        if (!ReadRating(prof.rating))
        {
            return;
        }

        const bool uploaded = !m_UploadedAvatar.empty();
        prof.avatar = AvatarForSave(prof.name); // Uploaded or random image stored as BLOB

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

        SetStatus(uploaded ? _T("Record inserted with the uploaded image and logged to AuditLogs.")
            : _T("Record inserted with a random image and logged to AuditLogs."));
        ClearInputFields();
        RefreshGridData();
    }

    // Parses the Rating field as a number (REAL column). An empty field means 0.
    // Shows a message and returns false if the text isn't a valid number.
    bool ReadRating(double& outRating)
    {
        CString text;
        m_EditRating.GetWindowText(text);
        text.Trim();
        if (text.IsEmpty())
        {
            outRating = 0.0;
            return true;
        }

        LPTSTR pEnd = nullptr;
        const double value = _tcstod(text, &pEnd);
        if (pEnd == text.GetString() || *pEnd != _T('\0') || !std::isfinite(value))
        {
            AfxMessageBox(_T("Rating must be a number, for example 4 or 4.75."));
            m_EditRating.SetFocus();
            m_EditRating.SetSel(0, -1);
            return false;
        }

        outRating = value;
        return true;
    }

    void UpdateProfile()
    {
        if (m_SelectedId < 0)
        {
            return;
        }

        Profile prof;
        prof.id = m_SelectedId;
        m_EditName.GetWindowText(prof.name);
        m_EditEngine.GetWindowText(prof.engine);
        prof.name.Trim();
        prof.engine.Trim();

        if (prof.name.IsEmpty())
        {
            return;
        }

        if (!ReadRating(prof.rating))
        {
            return;
        }

        const bool uploaded = !m_UploadedAvatar.empty();
        prof.avatar = AvatarForSave(prof.name); // Uploaded image, or a fresh random one

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
        status.Format(_T("Record %lld updated with %s image."), m_SelectedId,
            uploaded ? _T("the uploaded") : _T("a random"));
        SetStatus(status);
        ClearInputFields();
        RefreshGridData();
    }

    void DeleteProfile()
    {
        if (m_SelectedId < 0)
        {
            return;
        }

        CString confirmMsg;
        confirmMsg.Format(_T("Delete record %lld?"), m_SelectedId);
        if (AfxMessageBox(confirmMsg, MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

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
        m_EditRating.SetWindowText(_T(""));
        m_UploadedAvatar.clear();
        m_SelectedId = -1;
        m_GridCtrl.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ShowPreview(-1);
        UpdateButtonStates();
    }

    void SetStatus(const CString& text)
    {
        if (m_StaticStatus.GetSafeHwnd())
        {
            m_StaticStatus.SetWindowText(text);
        }
    }

    void UpdateButtonStates()
    {
        const BOOL hasSelection = (m_SelectedId >= 0);
        if (m_BtnUpdate.GetSafeHwnd())
        {
            m_BtnUpdate.EnableWindow(hasSelection);
        }

        if (m_BtnDelete.GetSafeHwnd())
        {
            m_BtnDelete.EnableWindow(hasSelection);
        }

        if (m_BtnRandom.GetSafeHwnd())
        {
            m_BtnRandom.EnableWindow(!m_UploadedAvatar.empty());
        }
    }

    void OnGridSelectionChanged(int iItem)
    {
        if (iItem < 0)
        {
            m_SelectedId = -1;
            ShowPreview(-1);
            UpdateButtonStates();
            return;
        }

        CString strId = m_GridCtrl.GetItemText(iItem, 0);
        CString strName = m_GridCtrl.GetItemText(iItem, 1);
        CString strEngine = m_GridCtrl.GetItemText(iItem, 2);

        m_SelectedId = _ttoi64(strId);
        m_EditName.SetWindowText(strName);
        m_EditEngine.SetWindowText(strEngine);
        m_EditRating.SetWindowText(m_GridCtrl.GetItemText(iItem, 3));

        ShowPreview(iItem);

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

            case IDC_BTN_UPLOAD:
                UploadImage();
                return TRUE;

            case IDC_BTN_RANDOM:
                DiscardUploadedImage();
                SetStatus(_T("Uploaded image discarded; a random image will be used."));
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
