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

set CXXFLAGS=-O2 -Wall -fno-exceptions -fno-rtti -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-strlen -fno-builtin-strcmp -fno-builtin-sin -fno-builtin-cos -Isrc -Isrc\\core -Isrc\\gui -Isrc\\order -Isrc\\sidebar -Isrc\\preview -Isrc\\perf -Isrc\\misc -c

if /i "%~1"=="/clean" (
  if exist obj rmdir /s /q obj
)

set NEEDFULL=0
if not exist obj mkdir obj
for %%N in (MOExperience SyringeHooks GdiText Lang MsgText SWPanel PowerDisplay VecText ToolTip ChatEdit FullMap ScaleBar PauseView Shot FrameCap GameSpeed PiP GridView SideScroll EscMenu PlayerColor LightingPreset SidebarSkin TypeSelect AirWay BuildQueue PatrolCircle RallyPath SmartOrder TreeChop FuzzyAttack AttackMove SmartArea OrderUtil MindSelect Observer DeployPreview WeaponRange SWSBar SWSTest RepairAll PlaceScatter PlacementPreview OrderThrottle WaypointCap MegaListCap NoApproach PvpNotice Aggressive Icon PcxImage Weather) do (
  if not exist "obj\%%N.o" set NEEDFULL=1
)
if not exist "obj\icons.res.o" set NEEDFULL=1
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
"%PWSH%" -NoProfile -Command "$src = @(Get-ChildItem src -Include *.cpp -Recurse); $hdr = @(Get-ChildItem src -Include *.h -Recurse); $newest = ($hdr | Measure-Object LastWriteTime -Maximum).Maximum; $inc = @('-Isrc','-Isrc\core','-Isrc\gui','-Isrc\order','-Isrc\sidebar','-Isrc\preview','-Isrc\perf','-Isrc\misc'); $stale = @(foreach ($f in $src) { $o = 'obj\' + $f.BaseName + '.o'; if (-not (Test-Path $o)) { $f; continue }; $ot = (Get-Item $o).LastWriteTime; if ($f.LastWriteTime -gt $ot) { $f; continue }; if ($ot -lt $newest) { $dep = (& g++ -MM -MG @inc $f.FullName 2>$null) -join ' '; $hit = $false; foreach ($t in ($dep -split '\s+')) { if ($t -match '\.h$' -and (Test-Path $t) -and (Get-Item $t).LastWriteTime -gt $ot) { $hit = $true; break } }; if ($hit) { $f } } }); if ($stale.Count -gt 0) { $stale | ForEach-Object { $_.FullName } | Set-Content -Encoding ascii obj\stale.txt } else { Set-Content -Encoding ascii -Value '' obj\stale.txt }"

if exist obj\compiled.flag del obj\compiled.flag
if exist obj\res.flag del obj\res.flag >nul 2>nul
"%PWSH%" -NoProfile -Command "$out='obj\icons.res.o'; $f=@(Get-Item 'src\gui\icons.rc','src\gui\IconRes.h') + @(Get-ChildItem icons -Filter *.pcx -File); $new=($f | Measure-Object LastWriteTime -Maximum).Maximum; if ($null -eq $new) { exit 0 }; $old=[datetime]::MinValue; if (Test-Path $out) { $old=(Get-Item $out).LastWriteTime }; if ($new -gt $old) { & windres -i src\gui\icons.rc -o $out; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; Set-Content -Encoding ascii -Value 1 obj\res.flag }"
if errorlevel 1 exit /b 1
if exist obj\res.flag (
  echo [res] icons.rc
  echo ok> obj\compiled.flag
  del obj\res.flag >nul 2>nul
)
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

rem Language files live in dist\MOExperienceResources: a key that is missing in the
rem game dir renders as an EMPTY menu line, so deploy them every build.
xcopy /Y /D /Q /I dist\MOExperienceResources\*.ini ..\MOExperienceResources\ >nul

if not exist obj\compiled.flag (
  echo [fast] nothing changed - skip
  del obj\stale.txt >nul 2>nul
  echo [fast] ok
  exit /b 0
)

echo [ld] linking
g++ -shared -o dist\MOExperience.dll obj\MOExperience.o obj\SyringeHooks.o obj\GdiText.o obj\Lang.o obj\MsgText.o obj\SWPanel.o obj\PowerDisplay.o obj\VecText.o obj\TypeSelect.o obj\AirWay.o obj\BuildQueue.o obj\PatrolCircle.o obj\RallyPath.o obj\SmartOrder.o obj\TreeChop.o obj\FuzzyAttack.o obj\AttackMove.o obj\SmartArea.o obj\OrderUtil.o obj\MindSelect.o obj\Observer.o obj\DeployPreview.o obj\WeaponRange.o obj\ToolTip.o obj\ChatEdit.o obj\FullMap.o obj\ScaleBar.o obj\PauseView.o obj\Shot.o obj\FrameCap.o obj\GameSpeed.o obj\PiP.o obj\GridView.o obj\SideScroll.o obj\SWSBar.o obj\SWSTest.o obj\RepairAll.o obj\PlaceScatter.o obj\PlacementPreview.o obj\OrderThrottle.o obj\WaypointCap.o obj\MegaListCap.o obj\NoApproach.o obj\PvpNotice.o obj\Aggressive.o obj\EscMenu.o obj\PlayerColor.o obj\LightingPreset.o obj\SidebarSkin.o obj\Icon.o obj\PcxImage.o obj\Weather.o obj\icons.res.o ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -limm32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12 -Wl,-Map=obj\MOExperience.map
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

