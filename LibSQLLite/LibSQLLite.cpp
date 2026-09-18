// #define _AFXDLL // Required if you change project settings to shared library later

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
#include "LibSQLLite.h"

// sqlite3.c is compiled as a separate translation unit (LibSQLLite/sqlite3.c)
// to avoid duplicate symbol definitions.

// Implementations for DatabaseContext are placed here so sqlite3 symbols
// are available (sqlite3.c is compiled into this TU).

DatabaseContext::DatabaseContext()
    : m_pDb(nullptr)
{
}

DatabaseContext::~DatabaseContext()
{
    Close();
}

bool DatabaseContext::Open(const CString& dbPath, CString& outErrMsg)
{
    Close();

#ifdef UNICODE
    int rc = sqlite3_open16(static_cast<LPCWSTR>(dbPath), &m_pDb);
#else
    int rc = sqlite3_open(static_cast<LPCSTR>(dbPath), &m_pDb);
#endif
    if (rc != SQLITE_OK || m_pDb == nullptr)
    {
        outErrMsg = _T("Fatal: Could not initialize database file!\n");
        if (m_pDb)
        {
            outErrMsg += CString(sqlite3_errmsg(m_pDb));
            Close();
        }
        else
        {
            outErrMsg += _T("(sqlite3_open returned success but did not provide a DB handle.)");
        }
        return false;
    }

    for (auto& repoInstaller : m_Installers)
    {
        if (!repoInstaller->CreateTable(m_pDb, outErrMsg))
        {
            Close();
            return false;
        }
    }

    return true;
}

void DatabaseContext::Close()
{
    if (m_pDb)
    {
        sqlite3_close(m_pDb);
        m_pDb = nullptr;
    }
}

