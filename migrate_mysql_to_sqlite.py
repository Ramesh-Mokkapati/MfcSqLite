"""
migrate_mysql_to_sqlite.py
Migrates all tables from a MySQL database into a SQLite database.

Usage:
    python migrate_mysql_to_sqlite.py [--host 127.0.0.1]
                                      [--user root] [--password root]
                                      [--db <schema>]   # omit to get an interactive list
                                      [--sqlite C:\\VDL\\vdl.db]

Defaults match the VDL product configuration.
If --db is omitted the script lists all non-system schemas from MySQL and
asks you to choose one interactively.
"""

import argparse
import sqlite3
import sys
import os
import decimal
import datetime
import mysql.connector

# System schemas that should never be migrated
_SYSTEM_SCHEMAS = {"information_schema", "mysql", "performance_schema", "sys"}


def _coerce(value):
    """Convert MySQL Python types that SQLite's binding layer doesn't accept."""
    if value is None:
        return None
    if isinstance(value, decimal.Decimal):
        return float(value)
    if isinstance(value, (datetime.datetime, datetime.date, datetime.time,
                          datetime.timedelta)):
        return str(value)
    if isinstance(value, bytearray):
        return bytes(value)
    # int, float, str, bytes pass straight through
    return value

# ── MySQL → SQLite type mapping ──────────────────────────────────────────────
_MYSQL_TO_SQLITE = {
    # integers
    "tinyint":   "INTEGER",
    "smallint":  "INTEGER",
    "mediumint": "INTEGER",
    "int":       "INTEGER",
    "integer":   "INTEGER",
    "bigint":    "INTEGER",
    "bit":       "INTEGER",
    "year":      "INTEGER",
    "bool":      "INTEGER",
    "boolean":   "INTEGER",
    # reals
    "float":     "REAL",
    "double":    "REAL",
    "decimal":   "REAL",
    "numeric":   "REAL",
    # blobs
    "blob":      "BLOB",
    "tinyblob":  "BLOB",
    "mediumblob":"BLOB",
    "longblob":  "BLOB",
    "binary":    "BLOB",
    "varbinary": "BLOB",
    # everything else → TEXT
}

def mysql_type_to_sqlite(mysql_col_type: str) -> str:
    base = mysql_col_type.lower().split("(")[0].strip()
    return _MYSQL_TO_SQLITE.get(base, "TEXT")


def get_create_table_sql(table: str, columns: list) -> str:
    """Build a CREATE TABLE IF NOT EXISTS statement from MySQL column info."""
    col_defs = []
    for col in columns:
        col_name  = col["COLUMN_NAME"]
        col_type  = mysql_type_to_sqlite(col["DATA_TYPE"])
        nullable  = "" if col["IS_NULLABLE"] == "YES" else " NOT NULL"
        default   = ""
        extra     = col.get("EXTRA", "")
        if "auto_increment" in extra.lower():
            # SQLite uses INTEGER PRIMARY KEY for autoincrement
            col_defs.append(f"  `{col_name}` INTEGER PRIMARY KEY AUTOINCREMENT")
            continue
        if col["COLUMN_DEFAULT"] is not None:
            val = col["COLUMN_DEFAULT"]
            if col_type in ("INTEGER", "REAL"):
                default = f" DEFAULT {val}"
            else:
                escaped = str(val).replace("'", "''")
                default = f" DEFAULT '{escaped}'"
        col_defs.append(f"  `{col_name}` {col_type}{nullable}{default}")
    return f"CREATE TABLE IF NOT EXISTS `{table}` (\n" + ",\n".join(col_defs) + "\n);"


def migrate(mysql_cfg: dict, sqlite_path: str, mysql_db: str):
    # ── ensure destination directory exists ──────────────────────────────────
    dest_dir = os.path.dirname(sqlite_path)
    if dest_dir and not os.path.exists(dest_dir):
        os.makedirs(dest_dir, exist_ok=True)
        print(f"Created directory: {dest_dir}")

    print(f"Connecting to MySQL  : {mysql_cfg['host']}  db={mysql_db}  user={mysql_cfg['user']}")
    my_conn = mysql.connector.connect(
        host=mysql_cfg["host"],
        port=mysql_cfg.get("port", 3306),
        user=mysql_cfg["user"],
        password=mysql_cfg["password"],
        database=mysql_db,
        use_unicode=True,
        charset="utf8mb4",
    )
    my_cur = my_conn.cursor(dictionary=True)

    print(f"Opening SQLite       : {sqlite_path}")
    sq_conn = sqlite3.connect(sqlite_path)
    sq_conn.execute("PRAGMA journal_mode=WAL;")
    sq_conn.execute("PRAGMA synchronous=NORMAL;")
    sq_conn.execute("PRAGMA foreign_keys=OFF;")

    # ── enumerate tables ──────────────────────────────────────────────────────
    my_cur.execute("SHOW TABLES;")
    tables = [row[f"Tables_in_{mysql_db}"] for row in my_cur.fetchall()]
    print(f"\nFound {len(tables)} table(s) in `{mysql_db}`\n")

    total_rows = 0

    for table in tables:
        # ── schema ──────────────────────────────────────────────────────────
        my_cur.execute(
            "SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA "
            "FROM INFORMATION_SCHEMA.COLUMNS "
            "WHERE TABLE_SCHEMA = %s AND TABLE_NAME = %s "
            "ORDER BY ORDINAL_POSITION;",
            (mysql_db, table),
        )
        columns = my_cur.fetchall()
        if not columns:
            print(f"  {table}: no columns found, skipping")
            continue

        col_names = [c["COLUMN_NAME"] for c in columns]

        create_sql = get_create_table_sql(table, columns)
        sq_conn.execute(f"DROP TABLE IF EXISTS `{table}`;")
        sq_conn.execute(create_sql)

        # ── data ─────────────────────────────────────────────────────────────
        my_cur.execute(f"SELECT * FROM `{table}`;")
        rows = my_cur.fetchall()
        row_count = len(rows)

        if row_count == 0:
            print(f"  {table}: 0 rows (table created, no data)")
            sq_conn.commit()
            continue

        placeholders = ", ".join(["?"] * len(col_names))
        quoted_cols  = ", ".join([f"`{c}`" for c in col_names])
        insert_sql   = f"INSERT OR IGNORE INTO `{table}` ({quoted_cols}) VALUES ({placeholders});"

        # Per-column SQLite type — needed to preserve binary-in-char columns
        col_sqlite_types = [mysql_type_to_sqlite(c["DATA_TYPE"]) for c in columns]

        batch = []
        BATCH_SIZE = 500
        for row in rows:
            coerced = []
            for col_name, sq_type in zip(col_names, col_sqlite_types):
                val = _coerce(row[col_name])
                # Empty string in a numeric column should be NULL, not '' (TEXT).
                # MySQL loose typing can produce '' for missing INTEGER/REAL values;
                # SQLite would store it as TEXT, breaking numeric comparisons.
                if sq_type in ("INTEGER", "REAL") and val == "":
                    val = None
                if sq_type == "TEXT":
                    if isinstance(val, (bytes, bytearray)):
                        # use_unicode=False connector path: keep raw bytes as BLOB
                        val = bytes(val)
                    elif isinstance(val, str) and col_name == "UserPassword":
                        # UserPassword stores raw XOR-encrypted bytes in a char(32)
                        # column.  A utf8mb4 connection re-encodes those bytes as
                        # multi-byte Unicode; encoding back to cp1252 recovers the
                        # original byte sequence.  SQLite stores Python bytes as
                        # BLOB, and sqlite3_column_text() on a BLOB returns the raw
                        # bytes unchanged — matching what the C++ code expects.
                        try:
                            val = val.encode("cp1252")
                        except UnicodeEncodeError:
                            pass  # unusual: keep as TEXT
                coerced.append(val)
            batch.append(tuple(coerced))
            if len(batch) >= BATCH_SIZE:
                sq_conn.executemany(insert_sql, batch)
                batch.clear()
        if batch:
            sq_conn.executemany(insert_sql, batch)

        sq_conn.commit()
        total_rows += row_count
        print(f"  {table}: {row_count} row(s) migrated")

    sq_conn.close()
    my_cur.close()
    my_conn.close()

    print(f"\nDone. {len(tables)} table(s), {total_rows} row(s) written to {sqlite_path}")


def list_user_schemas(mysql_cfg: dict) -> list:
    """Connect to MySQL (no database selected) and return non-system schema names."""
    conn = mysql.connector.connect(
        host=mysql_cfg["host"],
        port=mysql_cfg.get("port", 3306),
        user=mysql_cfg["user"],
        password=mysql_cfg["password"],
        use_unicode=True,
        charset="utf8mb4",
    )
    cur = conn.cursor()
    cur.execute("SHOW DATABASES;")
    schemas = [row[0] for row in cur.fetchall()
               if row[0].lower() not in _SYSTEM_SCHEMAS]
    cur.close()
    conn.close()
    return schemas


def pick_schema(mysql_cfg: dict, default: str) -> str:
    """
    Return the schema to migrate.
    If --db was supplied use it directly (after confirming it exists).
    Otherwise list available schemas and prompt the user to choose.
    """
    schemas = list_user_schemas(mysql_cfg)

    if not schemas:
        print("No user schemas found in MySQL.", file=sys.stderr)
        sys.exit(1)

    if default:
        if default not in schemas:
            print(f"Schema '{default}' not found. Available schemas:", file=sys.stderr)
            for s in schemas:
                print(f"  {s}", file=sys.stderr)
            sys.exit(1)
        return default

    print("\nAvailable MySQL schemas:")
    for idx, name in enumerate(schemas, 1):
        print(f"  [{idx}] {name}")

    while True:
        raw = input("\nEnter schema number or name: ").strip()
        if raw.isdigit():
            choice = int(raw)
            if 1 <= choice <= len(schemas):
                return schemas[choice - 1]
            print(f"  Please enter a number between 1 and {len(schemas)}.")
        elif raw in schemas:
            return raw
        else:
            print(f"  '{raw}' is not in the list above.")


def main():
    parser = argparse.ArgumentParser(description="Migrate MySQL → SQLite")
    parser.add_argument("--host",     default="127.0.0.1")
    parser.add_argument("--port",     default=3306, type=int)
    parser.add_argument("--user",     default="root")
    parser.add_argument("--password", default="root")
    parser.add_argument("--db",       default="",             help="MySQL schema name (omit to choose interactively)")
    parser.add_argument("--sqlite",   default=r"C:\VDL\vdl.db", help="Destination SQLite file")
    args = parser.parse_args()

    mysql_cfg = {
        "host":     args.host,
        "port":     args.port,
        "user":     args.user,
        "password": args.password,
    }

    try:
        db_name = pick_schema(mysql_cfg, args.db)
        migrate(mysql_cfg, args.sqlite, db_name)
    except mysql.connector.Error as exc:
        print(f"\nMySQL error: {exc}", file=sys.stderr)
        sys.exit(1)
    except sqlite3.Error as exc:
        print(f"\nSQLite error: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCancelled.")
        sys.exit(0)
    except Exception as exc:
        print(f"\nError: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
