# MFC + SQLite Standalone Demo Project
This project is configured for Visual Studio 2022 (Community or professional).

## IMPORTANT STEP BEFORE RUNNING:
The 'sqlite3.c' and 'sqlite3.h' files included here are empty skeleton stubs to save download space.
To compile correctly:
1. Go to https://sqlite.org/download.html
2. Download the 'sqlite-amalgamation-*.zip' source code package.
3. Extract 'sqlite3.c' and 'sqlite3.h' from that zip, and overwrite the files inside this directory.

## Build Properties Set:
- Static Linking for MFC (Removes DLL reliance)
- Runtime Library configured to /MT and /MTd
- 'sqlite3.c' set to "Not Using Precompiled Headers"
