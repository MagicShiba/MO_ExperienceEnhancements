@echo off
rem ============================================================================
rem  build-full.bat  -  full build, but per-file so obj\ cache is populated
rem  (build-fast.bat reuses those .o files afterwards)
rem  ASCII only: cmd.exe reads .bat in OEM codepage.
rem ============================================================================
setlocal
set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
cd /d "%~dp0"

set CXXFLAGS=-O2 -Wall -fno-exceptions -fno-rtti -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-strlen -fno-builtin-strcmp -fno-builtin-sin -fno-builtin-cos -Isrc -Isrc\\GUI -Iicons -c

if not exist obj mkdir obj

for %%F in (src\MOExperience.cpp src\GUI\GdiText.cpp src\GUI\MsgText.cpp src\GUI\SWPanel.cpp src\GUI\PowerDisplay.cpp src\GUI\VecText.cpp src\GUI\ToolTip.cpp src\GUI\ChatEdit.cpp src\GUI\FullMap.cpp src\GUI\ScaleBar.cpp src\GUI\PauseView.cpp src\GUI\GridView.cpp src\GUI\SideScroll.cpp src\TypeSelect.cpp src\AirWay.cpp src\BuildQueue.cpp src\PatrolCircle.cpp src\SmartOrder.cpp src\TreeChop.cpp src\FuzzyAttack.cpp src\AttackMove.cpp src\SmartArea.cpp src\OrderUtil.cpp src\MindSelect.cpp src\Observer.cpp src\DeployPreview.cpp src\WeaponRange.cpp src\SWSBar.cpp src\RepairAll.cpp src\PlaceScatter.cpp src\OrderThrottle.cpp icons\Icon.cpp) do (
  echo [cc] %%~nxF
  g++ %CXXFLAGS% -o "obj\%%~nF.o" %%F
  if errorlevel 1 exit /b 1
)

echo [ld] linking
g++ -shared -o dist\MOExperience.dll obj\MOExperience.o obj\GdiText.o obj\MsgText.o obj\SWPanel.o obj\PowerDisplay.o obj\VecText.o obj\TypeSelect.o obj\AirWay.o obj\BuildQueue.o obj\PatrolCircle.o obj\SmartOrder.o obj\TreeChop.o obj\FuzzyAttack.o obj\AttackMove.o obj\SmartArea.o obj\OrderUtil.o obj\MindSelect.o obj\Observer.o obj\DeployPreview.o obj\WeaponRange.o obj\ToolTip.o obj\ChatEdit.o obj\FullMap.o obj\ScaleBar.o obj\PauseView.o obj\GridView.o obj\SideScroll.o obj\SWSBar.o obj\RepairAll.o obj\PlaceScatter.o obj\OrderThrottle.o obj\Icon.o ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -limm32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
if errorlevel 1 exit /b 1

copy /Y dist\MOExperience.dll ..\MOExperience.dll >nul
if errorlevel 1 exit /b 1
copy /Y dist\MOExperience.dll.inj ..\MOExperience.dll.inj >nul
if errorlevel 1 exit /b 1
echo [full] ok
