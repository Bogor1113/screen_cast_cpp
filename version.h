#pragma once
// version.h -- single source of the version number. build.bat / pack.ps1 call scripts\version_bump.ps1
// which increments and writes it back here. version format: major.minor. increment rule: if a
// dist\version.txt exists, bump the minor version from it; otherwise use this file as-is.
// All UI titles / installer-displayed versions reference kScreenCastVersion, so one change syncs everywhere.
#define SCREENCAST_VERSION_STR "1.21"
#define SCREENCAST_VERSION_WS  L"1.21"
