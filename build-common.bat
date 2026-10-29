@echo off
rem ============================================================================
rem  build-common.bat - build environment shared by build-fast / build-full.
rem  Call it (do not run it): it sets MINGW / PWSH / SRCINC / CXXFLAGS / OBJS,
rem  refreshes obj\sources.txt and deploys the language files.
rem  Needs nothing from the caller (no delayed expansion).
rem
rem  The source list is DERIVED, never hand-maintained: it is every .cpp under
rem  src\ (the repo layout rules say src\ holds build sources only). Adding,
rem  renaming or moving a .cpp needs no edit in any build script; only a NEW
rem  SUBDIRECTORY needs one entry in SRCINC below.
rem  ASCII only: cmd.exe reads .bat in OEM codepage.
rem ============================================================================
cd /d "%~dp0"

rem --- toolchain -------------------------------------------------------------
rem No absolute paths anywhere. Priority: %MOE_MINGW% (bin dir) > a toolchain
rem vendored in this repo > the sibling AutoLoadDev checkout > g++ on PATH.
set "MINGW="
if defined MOE_MINGW if not exist "%MOE_MINGW%\g++.exe" (
  echo [build] ERROR: MOE_MINGW is set but "%MOE_MINGW%\g++.exe" is missing.
  exit /b 1
)
if defined MOE_MINGW set "MINGW=%MOE_MINGW%"
for %%D in ("%~dp0tools\mingw\mingw32\bin" "%~dp0..\AutoLoadDev\tools\mingw\mingw32\bin") do (
  if not defined MINGW if exist "%%~fD\g++.exe" set "MINGW=%%~fD"
)
if defined MINGW set "PATH=%MINGW%;%PATH%"
if defined MINGW echo [env] mingw %MINGW%
if defined MINGW goto :mingw_ok
where g++ >nul 2>nul
if errorlevel 1 (
  echo [build] ERROR: g++ not found.
  echo [build]   put MinGW-w64 at tools\mingw\mingw32\bin, or set MOE_MINGW
  echo [build]   to its bin directory, or add it to PATH.
  exit /b 1
)
echo [env] mingw from PATH
:mingw_ok

rem --- powershell ------------------------------------------------------------
rem pwsh 7 preferred (faster start), Windows PowerShell as the always-present
rem fallback; both one-liners below are 5.1-compatible.
set "PWSH=powershell.exe"
where pwsh >nul 2>nul && set "PWSH=pwsh"
if "%PWSH%"=="powershell.exe" if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "PWSH=%ProgramFiles%\PowerShell\7\pwsh.exe"

rem Header lookup for the bare-name includes; keep in sync with src\ subdirs.
set "SRCINC=-Isrc -Isrc\core -Isrc\gui -Isrc\order -Isrc\sidebar -Isrc\preview -Isrc\perf -Isrc\misc"

set "CXXFLAGS=-O2 -Wall -fno-exceptions -fno-rtti -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-strlen -fno-builtin-strcmp -fno-builtin-sin -fno-builtin-cos %SRCINC% -c"

if not exist obj mkdir obj

rem obj\sources.txt: one path per line, relative to this dir, sorted so that
rem compile and link order stay identical between runs.
"%PWSH%" -NoProfile -Command "$r=(Get-Location).Path.Length+1; Get-ChildItem src -Recurse -File -Filter *.cpp | Sort-Object FullName | ForEach-Object { $_.FullName.Substring($r) } | Set-Content -Encoding ascii obj\sources.txt"
if errorlevel 1 exit /b 1

rem Object list for the link line, derived from that same list.
rem call set = double expansion, so this works without EnableDelayedExpansion.
set "OBJS="
for /f "usebackq delims=" %%F in ("obj\sources.txt") do call set "OBJS=%%OBJS%% obj\%%~nF.o"

rem Language files live in dist\MOExperienceResources: a key that is missing in
rem the game dir renders as an EMPTY menu line, so deploy them every build.
xcopy /Y /D /Q /I "%~dp0dist\MOExperienceResources\*.ini" "%~dp0..\MOExperienceResources\" >nul
