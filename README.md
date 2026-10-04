# MFC + SQLite Standalone Demo

A Visual Studio 2022 demo project showing how to integrate SQLite into an MFC application using a generic, template-based ORM. The project is split into two components: a reusable library (`LibSQLLite`) and a demo MFC dialog app (`MfcSqliteApp`).

## Prerequisites

- Visual Studio 2022 (Community or Professional)
- Windows 10 or later

## Building

Open `MfcSqlite.sln` in Visual Studio 2022 and build the solution (Debug or Release). Both projects build as x64 by default.

Build settings applied to both projects:
- MFC statically linked (`/MT` / `/MTd`) — no MFC DLL dependency at runtime
- `sqlite3.c` compiled as a separate translation unit with precompiled headers disabled

## Project Structure

```
MfcSqlite.sln
├── LibSQLLite\               — Static library: ORM types + DatabaseContext
│   ├── LibSQLLite.h          — SqliteRepository<T>, TableSchema<T>, DatabaseContext
│   ├── LibSQLLite.cpp        — DatabaseContext::Open / Close implementation
│   ├── sqlite3.h             — SQLite amalgamation header
│   └── sqlite3.c             — SQLite amalgamation source
├── MfcSqliteApp\             — Demo MFC dialog application
│   └── main.cpp              — Profile + AuditLog schemas, CMainGridDialog, CMfcSqliteApp
└── migrate_mysql_to_sqlite.py — Standalone utility: migrate a MySQL schema to SQLite
```

## Architecture

### ORM Layer (`LibSQLLite`)

The library provides two template types and one manager class:

| Type | Purpose |
|---|---|
| `ColumnMapping<T>` | Maps a single C++ field to a SQLite column — holds bind and read lambdas |
| `TableSchema<T>` | Groups a table name, primary key accessors, and a list of `ColumnMapping<T>` |
| `SqliteRepository<T>` | Performs `GetAll`, `Insert`, `Update`, `Delete` against a given schema |
| `DatabaseContext` | Opens/closes the database and calls `CreateTable` on every registered repository |

All SQLite datatypes are supported: `INTEGER`, `REAL`, `TEXT`, `BLOB`, and `NULL`.

### Demo App (`MfcSqliteApp`)

The app creates two tables on first run and provides a `CListCtrl`-based grid UI:

- **Profiles** — `ID` (INTEGER PK), `Name` (TEXT), `Engine` (TEXT), `Rating` (REAL), `Avatar` (BLOB)
- **AuditLogs** — `LogID` (INTEGER PK), `Action` (TEXT), `ProfileID` (INTEGER)

Every insert, update, or delete on `Profiles` also writes a corresponding row to `AuditLogs`, demonstrating multi-table write coordination through a single `DatabaseContext`.

The database file (`mfc_app_multitable.db`) is created in the same directory as the compiled executable.

## Usage

Run the compiled executable. The dialog opens with a grid and four action buttons:

| Button | Behaviour |
|---|---|
| **Insert** | Creates a new Profile row from the Name/Engine fields (Rating and Avatar populated with demo values) |
| **Update Selected** | Overwrites the selected row's fields |
| **Delete Selected** | Deletes the selected row after confirmation |
| **Refresh Grid** | Reloads all rows from the database |
| **Clear Fields** | Clears the input fields and deselects the grid row |

---

## MySQL → SQLite Migration Script

`migrate_mysql_to_sqlite.py` is a standalone Python utility that copies every table from a MySQL schema into a SQLite database file. It is intended as a one-shot data-migration tool — for example, to seed a local SQLite file for use with the MFC app during development or offline testing.

### Dependencies

| Package | Install | Purpose |
|---|---|---|
| `mysql-connector-python` | `pip install mysql-connector-python` | MySQL client driver |

All other imports (`argparse`, `sqlite3`, `decimal`, `datetime`, `os`, `sys`) are part of the Python standard library. Python 3.8 or later is required.

### Usage

```bash
# Minimal — connects to localhost with default credentials, prompts to choose a schema
python migrate_mysql_to_sqlite.py

# Fully specified
python migrate_mysql_to_sqlite.py \
  --host 127.0.0.1 \
  --port 3306 \
  --user root \
  --password root \
  --db my_schema \
  --sqlite C:\my.db
```

If `--db` is omitted the script lists all non-system schemas from MySQL and asks you to pick one interactively.

### Arguments

| Argument | Default | Description |
|---|---|---|
| `--host` | `127.0.0.1` | MySQL server hostname or IP |
| `--port` | `3306` | MySQL server port |
| `--user` | `root` | MySQL username |
| `--password` | `root` | MySQL password |
| `--db` | _(interactive)_ | MySQL schema to migrate; omit to choose from a list |
| `--sqlite` | `C:\my.db` | Destination SQLite file path (created if it does not exist) |

### Type mapping

| MySQL type(s) | SQLite storage |
|---|---|
| `TINYINT`, `SMALLINT`, `MEDIUMINT`, `INT`, `BIGINT`, `BIT`, `YEAR`, `BOOL` | `INTEGER` |
| `FLOAT`, `DOUBLE`, `DECIMAL`, `NUMERIC` | `REAL` |
| `BLOB`, `TINYBLOB`, `MEDIUMBLOB`, `LONGBLOB`, `BINARY`, `VARBINARY` | `BLOB` |
| All other types (`VARCHAR`, `CHAR`, `TEXT`, `ENUM`, `DATE`, `DATETIME`, …) | `TEXT` |

`AUTO_INCREMENT` columns are mapped to `INTEGER PRIMARY KEY AUTOINCREMENT`. `DECIMAL` and date/time values are coerced to Python `float` and `str` respectively before binding, as SQLite's Python driver does not accept those types directly.

---

## Extending the ORM

To add a new table, define a struct, write a `Make*Schema()` factory that returns a `TableSchema<YourStruct>`, instantiate a `SqliteRepository<YourStruct>`, and register it with `DatabaseContext::RegisterRepository` before calling `Open`. No changes to the library are needed.
