@echo off
rem ============================================================================
rem  build-fast.bat  -  incremental build (recompile only changed .cpp)
rem  ASCII only: cmd.exe reads .bat in OEM codepage; UTF-8 CJK breaks it.
rem  First run: use the full build once (it creates obj\). After that use this.
rem    build-fast.bat          incremental
rem    build-fast.bat /clean   wipe obj, full rebuild next time
rem ============================================================================
setlocal
set "PWSH=C:\Program Files\PowerShell\7\pwsh.exe"
set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
cd /d "%~dp0"

set CXXFLAGS=-O2 -Wall -fno-exceptions -fno-rtti -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-strlen -fno-builtin-strcmp -Isrc -Iicons -c

if /i "%~1"=="/clean" (
  if exist obj rmdir /s /q obj
)

set NEEDFULL=0
if not exist obj mkdir obj
for %%N in (MOExperience GdiText VecText TypeSelect AirWay BuildQueue PatrolCircle SmartOrder TreeChop FuzzyAttack SmartArea OrderUtil MindSelect DeployPreview ToolTip ChatEdit SWSBar RepairAll Icon) do (
  if not exist "obj\%%N.o" set NEEDFULL=1
)
if "%NEEDFULL%"=="1" (
  echo [fast] no obj cache - running full build
  call build-full.bat
  exit /b %errorlevel%
)

"%PWSH%" -NoProfile -Command "$src = @(Get-ChildItem src\*.cpp) + @(Get-Item icons\Icon.cpp); $hdr = @(Get-ChildItem src\*.h); $stale = @(foreach ($f in $src) { $o = 'obj\' + $f.BaseName + '.o'; if (-not (Test-Path $o)) { $f; continue }; $ot = (Get-Item $o).LastWriteTime; if ($f.LastWriteTime -gt $ot) { $f } elseif ($hdr | Where-Object { $_.LastWriteTime -gt $ot }) { $f } }); if ($stale.Count -gt 0) { $stale | ForEach-Object { $_.FullName } | Set-Content -Encoding ascii obj\stale.txt } else { Set-Content -Encoding ascii -Value '' obj\stale.txt }"

if exist obj\compiled.flag del obj\compiled.flag
rem NOTE: keep obj\stale.txt existing even when empty. "for /f in" errors out
rem with "file not found" (and looks like a failure) if the list is absent.

for /f "usebackq delims=" %%F in ("obj\stale.txt") do (
  if not "%%F"=="" (
    echo [cc] %%~nxF
    g++ %CXXFLAGS% -o "obj\%%~nF.o" "%%F"
    if errorlevel 1 exit /b 1
    echo ok> obj\compiled.flag
  )
)

if not exist obj\compiled.flag (
  echo [fast] nothing changed - skip
  del obj\stale.txt >nul 2>nul
  echo [fast] ok
  exit /b 0
)

echo [ld] linking
g++ -shared -o dist\MOExperience.dll obj\MOExperience.o obj\GdiText.o obj\VecText.o obj\TypeSelect.o obj\AirWay.o obj\BuildQueue.o obj\PatrolCircle.o obj\SmartOrder.o obj\TreeChop.o obj\FuzzyAttack.o obj\SmartArea.o obj\OrderUtil.o obj\MindSelect.o obj\DeployPreview.o obj\ToolTip.o obj\ChatEdit.o obj\SWSBar.o obj\RepairAll.o obj\Icon.o ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -limm32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
if errorlevel 1 exit /b 1

del obj\stale.txt >nul 2>nul
del obj\compiled.flag >nul 2>nul
copy /Y dist\MOExperience.dll ..\MOExperience.dll >nul
if errorlevel 1 (
  echo [fast] ERROR: cannot copy to ..\MOExperience.dll - is the game running?
  exit /b 1
)
copy /Y dist\MOExperience.dll.inj ..\MOExperience.dll.inj >nul
if errorlevel 1 exit /b 1
echo [fast] ok
