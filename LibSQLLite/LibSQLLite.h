#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Target Windows 10
#endif

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers

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

// ============================================================================
// --- EXTENDED ORM TYPES & ALL SQLITE DATATYPES (C++11) ---
// ============================================================================

// https://www.sqlite.org/datatype3.html - 2.2 Storage Classes and Datatypes
enum class ColumnType
{
    Integer,
    Real,
    Text,
    Blob
};

template <typename T>
struct ColumnMapping
{
    std::string columnName;
    ColumnType type;
    std::function<void(const T&, sqlite3_stmt*, int index)> bindParam;
    std::function<void(T&, sqlite3_stmt*, int index)> readColumn;
};

template <typename T>
struct TableSchema
{
    std::string tableName;
    std::string primaryKeyName;
    std::function<int64_t(const T&)> getPrimaryKey;
    std::function<void(T&, int64_t)> setPrimaryKey;
    std::vector<ColumnMapping<T>> columns;
};

// Interface for auto-creating schema across multi-table registration
class ITableSchemaInstaller
{
public:
    virtual ~ITableSchemaInstaller() = default;
    virtual bool CreateTable(sqlite3* pDb, CString& outErrMsg) = 0;
};

// ============================================================================
// --- GENERIC REPOSITORY CLASS ---
// ============================================================================

template <typename T>
class SqliteRepository : public ITableSchemaInstaller
{
public:
    explicit SqliteRepository(TableSchema<T> schema)
        : m_Schema(std::move(schema)), m_pDb(nullptr) {
    }

    void SetDbHandle(sqlite3* pDb) { m_pDb = pDb; }

    bool CreateTable(sqlite3* pDb, CString& outErrMsg) override
    {
        m_pDb = pDb; // Cache connection handle inside repository

        std::string sql = "CREATE TABLE IF NOT EXISTS " + m_Schema.tableName + " (" +
            m_Schema.primaryKeyName + " INTEGER PRIMARY KEY AUTOINCREMENT";

        for (const auto& col : m_Schema.columns)
        {
            std::string typeStr;
            switch (col.type)
            {
            case ColumnType::Integer: typeStr = " INTEGER"; break;
            case ColumnType::Real:    typeStr = " REAL";    break;
            case ColumnType::Text:    typeStr = " TEXT";    break;
            case ColumnType::Blob:    typeStr = " BLOB";    break;
            }
            sql += ", " + col.columnName + typeStr;
        }
        sql += ");";

        char* pErrMsg = nullptr;
        int rc = sqlite3_exec(pDb, sql.c_str(), nullptr, nullptr, &pErrMsg);
        if (rc != SQLITE_OK)
        {
            outErrMsg = _T("Failed auto-creating table ");
            outErrMsg += m_Schema.tableName.c_str();
            if (pErrMsg)
            {
                outErrMsg += _T(":\n");
                outErrMsg += CString(pErrMsg);
                sqlite3_free(pErrMsg);
            }
            return false;
        }
        return true;
    }

    bool GetAll(std::vector<T>& outRecords, CString& outErrMsg)
    {
        outRecords.clear();
        if (!m_pDb)
        {
            outErrMsg = _T("Database handle is invalid/null.");
            return false;
        }

        std::string sql = "SELECT " + m_Schema.primaryKeyName;
        for (const auto& col : m_Schema.columns)
        {
            sql += ", " + col.columnName;
        }
        sql += " FROM " + m_Schema.tableName + " ORDER BY " + m_Schema.primaryKeyName + " DESC;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_pDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            outErrMsg = CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            T record{};
            m_Schema.setPrimaryKey(record, sqlite3_column_int64(stmt, 0));

            int colIdx = 1;
            for (const auto& col : m_Schema.columns)
            {
                if (sqlite3_column_type(stmt, colIdx) != SQLITE_NULL)
                {
                    col.readColumn(record, stmt, colIdx);
                }
                colIdx++;
            }

            outRecords.push_back(record);
        }

        sqlite3_finalize(stmt);
        return true;
    }

    bool Insert(T& record, CString& outErrMsg)
    {
        if (!m_pDb)
        {
            outErrMsg = _T("Database handle is invalid/null.");
            return false;
        }

        std::string sql = "INSERT INTO " + m_Schema.tableName + " (";
        std::string values = " VALUES (";

        for (size_t i = 0; i < m_Schema.columns.size(); ++i)
        {
            sql += m_Schema.columns[i].columnName + (i == m_Schema.columns.size() - 1 ? "" : ", ");
            values += "?" + std::string(i == m_Schema.columns.size() - 1 ? "" : ", ");
        }
        sql += ")" + values + ");";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_pDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            outErrMsg = _T("Insert failed: Preparation error.\n") + CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        int bindIdx = 1;
        for (const auto& col : m_Schema.columns)
        {
            col.bindParam(record, stmt, bindIdx++);
        }

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            outErrMsg = _T("Insert failed.\n") + CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        m_Schema.setPrimaryKey(record, sqlite3_last_insert_rowid(m_pDb));
        return true;
    }

    bool Update(const T& record, CString& outErrMsg)
    {
        if (!m_pDb)
        {
            outErrMsg = _T("Database handle is invalid/null.");
            return false;
        }

        std::string sql = "UPDATE " + m_Schema.tableName + " SET ";
        for (size_t i = 0; i < m_Schema.columns.size(); ++i)
        {
            sql += m_Schema.columns[i].columnName + " = ?" + (i == m_Schema.columns.size() - 1 ? "" : ", ");
        }
        sql += " WHERE " + m_Schema.primaryKeyName + " = ?;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_pDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            outErrMsg = _T("Update failed: Preparation error.\n") + CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        int bindIdx = 1;
        for (const auto& col : m_Schema.columns)
        {
            col.bindParam(record, stmt, bindIdx++);
        }
        sqlite3_bind_int64(stmt, bindIdx, m_Schema.getPrimaryKey(record));

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            outErrMsg = _T("Update failed.\n") + CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        return true;
    }

    bool Delete(int64_t primaryKey, CString& outErrMsg)
    {
        if (!m_pDb)
        {
            outErrMsg = _T("Database handle is invalid/null.");
            return false;
        }

        std::string sql = "DELETE FROM " + m_Schema.tableName + " WHERE " + m_Schema.primaryKeyName + " = ?;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_pDb, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            outErrMsg = _T("Delete failed: Preparation error.\n") + CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        sqlite3_bind_int64(stmt, 1, primaryKey);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE)
        {
            outErrMsg = _T("Delete failed.\n") + CString(sqlite3_errmsg(m_pDb));
            return false;
        }

        return true;
    }

private:
    TableSchema<T> m_Schema;
    sqlite3* m_pDb;
};

// ============================================================================
// --- MULTI-TABLE DATABASE CONTEXT ---
// ============================================================================

class DatabaseContext
{
public:
    DatabaseContext();
    ~DatabaseContext();

    template <typename T>
    void RegisterRepository(SqliteRepository<T>& repo)
    {
        m_Installers.push_back(&repo);
        if (m_pDb)
        {
            repo.SetDbHandle(m_pDb);
        }
    }

    bool Open(const CString& dbPath, CString& outErrMsg);
    void Close();

private:
    sqlite3* m_pDb;
    std::vector<ITableSchemaInstaller*> m_Installers;
};
