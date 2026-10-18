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

set CXXFLAGS=-O2 -Wall -fno-exceptions -fno-rtti -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-strlen -fno-builtin-strcmp -fno-builtin-sin -fno-builtin-cos -Isrc -Isrc\\GUI -Iicons -c

if /i "%~1"=="/clean" (
  if exist obj rmdir /s /q obj
)

set NEEDFULL=0
if not exist obj mkdir obj
for %%N in (MOExperience SyringeHooks GdiText MsgText SWPanel PowerDisplay VecText ToolTip ChatEdit FullMap ScaleBar PauseView GridView SideScroll TypeSelect AirWay BuildQueue PatrolCircle RallyPath SmartOrder TreeChop FuzzyAttack AttackMove SmartArea OrderUtil MindSelect Observer DeployPreview WeaponRange SWSBar RepairAll PlaceScatter OrderThrottle WaypointCap MegaListCap NoApproach Aggressive Icon IconMap) do (
  if not exist "obj\%%N.o" set NEEDFULL=1
)
if "%NEEDFULL%"=="1" (
  echo [fast] no obj cache - running full build
  call build-full.bat
  exit /b %errorlevel%
)

rem Rebuild a .cpp when it changed, or when a header IT ACTUALLY INCLUDES changed.
rem (Old rule "any header in src changed" forced a full rebuild on every edit to
rem  a widely-included header such as OrderUtil.h -- e.g. a MOE_VERSION bump.)
rem g++ -MM yields the per-file include list; only run it for files whose .o is
rem  older than the newest header, so the no-header-change path stays fast.
"%PWSH%" -NoProfile -Command "$src = @(Get-ChildItem src -Include *.cpp -Recurse) + @(Get-ChildItem icons\*.cpp); $hdr = @(Get-ChildItem src -Include *.h -Recurse); $newest = ($hdr | Measure-Object LastWriteTime -Maximum).Maximum; $inc = @('-Isrc','-Isrc\GUI','-Iicons'); $stale = @(foreach ($f in $src) { $o = 'obj\' + $f.BaseName + '.o'; if (-not (Test-Path $o)) { $f; continue }; $ot = (Get-Item $o).LastWriteTime; if ($f.LastWriteTime -gt $ot) { $f; continue }; if ($ot -lt $newest) { $dep = (& g++ -MM -MG @inc $f.FullName 2>$null) -join ' '; $hit = $false; foreach ($t in ($dep -split '\s+')) { if ($t -match '\.h$' -and (Test-Path $t) -and (Get-Item $t).LastWriteTime -gt $ot) { $hit = $true; break } }; if ($hit) { $f } } }); if ($stale.Count -gt 0) { $stale | ForEach-Object { $_.FullName } | Set-Content -Encoding ascii obj\stale.txt } else { Set-Content -Encoding ascii -Value '' obj\stale.txt }"

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
g++ -shared -o dist\MOExperience.dll obj\MOExperience.o obj\SyringeHooks.o obj\GdiText.o obj\MsgText.o obj\SWPanel.o obj\PowerDisplay.o obj\VecText.o obj\TypeSelect.o obj\AirWay.o obj\BuildQueue.o obj\PatrolCircle.o obj\RallyPath.o obj\SmartOrder.o obj\TreeChop.o obj\FuzzyAttack.o obj\AttackMove.o obj\SmartArea.o obj\OrderUtil.o obj\MindSelect.o obj\Observer.o obj\DeployPreview.o obj\WeaponRange.o obj\ToolTip.o obj\ChatEdit.o obj\FullMap.o obj\ScaleBar.o obj\PauseView.o obj\GridView.o obj\SideScroll.o obj\SWSBar.o obj\RepairAll.o obj\PlaceScatter.o obj\OrderThrottle.o obj\WaypointCap.o obj\MegaListCap.o obj\NoApproach.o obj\Aggressive.o obj\Icon.o obj\IconMap.o ^
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
rem Hooks are embedded in .syhks00 now (no .inj needed, like AutoLoadDev).
rem A stale game-dir .inj would declare every hook twice, so remove it.
if exist ..\MOExperience.dll.inj del ..\MOExperience.dll.inj >nul
echo [fast] ok
