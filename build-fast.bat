@echo off
rem ============================================================================
rem  build-fast.bat  -  the one build implementation (incremental).
rem  Toolchain / flags / source list live in build-common.bat.
rem    build-fast.bat          recompile what changed, relink, deploy
rem    build-fast.bat /clean   wipe obj\ first (what build-full.bat does)
rem ============================================================================
setlocal
cd /d "%~dp0"

if /i "%~1"=="/clean" if exist obj rmdir /s /q obj

call "%~dp0build-common.bat"
if errorlevel 1 exit /b 1

rem A .cpp is stale when its .o is missing, when the .cpp is newer than the .o,
rem or when a header IT ACTUALLY INCLUDES is newer. (Old rule "any header in
rem src changed" forced a full rebuild on every edit to a widely included
rem header such as OrderUtil.h -- e.g. a MOE_VERSION bump.)
rem g++ -MM yields the per-file include list; it runs only for files whose .o is
rem older than the newest header, so the no-header-change path stays fast.
"%PWSH%" -NoProfile -Command "$src = @(Get-Content obj\sources.txt | Where-Object { $_ } | ForEach-Object { Get-Item -LiteralPath $_ }); $hdr = @(Get-ChildItem src -Recurse -File -Filter *.h); $newest = ($hdr | Measure-Object LastWriteTime -Maximum).Maximum; $inc = $env:SRCINC -split ' '; $stale = @(foreach ($f in $src) { $o = 'obj\' + $f.BaseName + '.o'; if (-not (Test-Path $o)) { $f; continue }; $ot = (Get-Item $o).LastWriteTime; if ($f.LastWriteTime -gt $ot) { $f; continue }; if ($ot -lt $newest) { $dep = (& g++ -MM -MG @inc $f.FullName 2>$null) -join ' '; $hit = $false; foreach ($t in ($dep -split '\s+')) { if ($t -match '\.h$' -and (Test-Path $t) -and (Get-Item $t).LastWriteTime -gt $ot) { $hit = $true; break } }; if ($hit) { $f } } }); if ($stale.Count -gt 0) { $stale | ForEach-Object { $_.FullName } | Set-Content -Encoding ascii obj\stale.txt } else { Set-Content -Encoding ascii -Value '' obj\stale.txt }"
if errorlevel 1 exit /b 1

set "LINK=0"

rem icons.res.o is rebuilt only when icons.rc / IconRes.h / an icons\*.pcx is
rem newer; the PS step drops obj\res.flag in that case (a missing .o counts as
rem infinitely old, so a wiped obj\ rebuilds the resource as well).
"%PWSH%" -NoProfile -Command "$out='obj\icons.res.o'; $f=@(Get-Item 'src\gui\icons.rc','src\gui\IconRes.h') + @(Get-ChildItem icons -Filter *.pcx -File); $new=($f | Measure-Object LastWriteTime -Maximum).Maximum; if ($null -eq $new) { exit 0 }; $old=[datetime]::MinValue; if (Test-Path $out) { $old=(Get-Item $out).LastWriteTime }; if ($new -gt $old) { & windres -i src\gui\icons.rc -o $out; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; Set-Content -Encoding ascii -Value 1 obj\res.flag }"
if errorlevel 1 exit /b 1
if exist obj\res.flag (
  del obj\res.flag >nul 2>nul
  echo [res] icons.rc
  set "LINK=1"
)

rem obj\stale.txt always exists (written empty when nothing is stale) and is
rem left behind after the build as the last rebuild list.
for /f "usebackq delims=" %%F in ("obj\stale.txt") do (
  echo [cc] %%~nxF
  g++ %CXXFLAGS% -o "obj\%%~nF.o" "%%F"
  if errorlevel 1 exit /b 1
  set "LINK=1"
)

if "%LINK%"=="0" (
  echo [build] nothing changed - skip
  exit /b 0
)

echo [ld] linking
g++ -shared -o dist\MOExperience.dll %OBJS% obj\icons.res.o ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -limm32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12 -Wl,-Map=obj\MOExperience.map
if errorlevel 1 exit /b 1

copy /Y dist\MOExperience.dll ..\MOExperience.dll >nul
if errorlevel 1 (
  echo [build] ERROR: cannot copy to ..\MOExperience.dll - is the game running?
  exit /b 1
)
rem Hooks are embedded in .syhks00 now (no .inj needed, like AutoLoadDev).
rem A stale game-dir .inj would declare every hook twice, so remove it.
if exist ..\MOExperience.dll.inj del ..\MOExperience.dll.inj >nul
echo [build] ok
