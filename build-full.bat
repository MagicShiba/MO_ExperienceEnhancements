@echo off
rem ============================================================================
rem  build-full.bat  -  full build, but per-file so obj\ cache is populated
rem  (build-fast.bat reuses those .o files afterwards)
rem  ASCII only: cmd.exe reads .bat in OEM codepage.
rem ============================================================================
setlocal
set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
cd /d "%~dp0"

set CXXFLAGS=-O2 -Wall -fno-exceptions -fno-rtti -fno-builtin-memset -fno-builtin-memcpy -fno-builtin-memmove -fno-builtin-strlen -fno-builtin-strcmp -fno-builtin-sin -fno-builtin-cos -Isrc -Isrc\\core -Isrc\\gui -Isrc\\order -Isrc\\sidebar -Isrc\\preview -Isrc\\perf -Isrc\\misc -c

if not exist obj mkdir obj

rem Language files live in dist\MOExperienceResources: a key that is missing in the
rem game dir renders as an EMPTY menu line, so deploy them every build.
xcopy /Y /D /Q /I dist\MOExperienceResources\*.ini ..\MOExperienceResources\ >nul

echo [res] icons.rc
windres -i src\gui\icons.rc -o obj\icons.res.o
if errorlevel 1 exit /b 1

for %%F in (src\core\MOExperience.cpp src\core\SyringeHooks.cpp src\core\Lang.cpp src\gui\GdiText.cpp src\gui\MsgText.cpp src\gui\SWPanel.cpp src\gui\PowerDisplay.cpp src\gui\VecText.cpp src\gui\ToolTip.cpp src\gui\ChatEdit.cpp src\gui\FullMap.cpp src\gui\ScaleBar.cpp src\gui\PauseView.cpp src\gui\Shot.cpp src\gui\PiP.cpp src\gui\GridView.cpp src\gui\EscMenu.cpp src\gui\PlayerColor.cpp src\gui\Icon.cpp src\gui\PcxImage.cpp src\order\TypeSelect.cpp src\order\AirWay.cpp src\order\PatrolCircle.cpp src\order\RallyPath.cpp src\order\SmartOrder.cpp src\order\TreeChop.cpp src\order\FuzzyAttack.cpp src\order\AttackMove.cpp src\order\SmartArea.cpp src\order\OrderUtil.cpp src\order\MindSelect.cpp src\order\OrderThrottle.cpp src\order\Aggressive.cpp src\sidebar\BuildQueue.cpp src\sidebar\SWSBar.cpp src\sidebar\SWSTest.cpp src\sidebar\RepairAll.cpp src\sidebar\SideScroll.cpp src\sidebar\SidebarSkin.cpp src\preview\DeployPreview.cpp src\preview\PlaceScatter.cpp src\preview\PlacementPreview.cpp src\preview\WeaponRange.cpp src\perf\FrameCap.cpp src\perf\GameSpeed.cpp src\misc\Observer.cpp src\misc\WaypointCap.cpp src\misc\MegaListCap.cpp src\misc\NoApproach.cpp src\misc\PvpNotice.cpp) do (
  echo [cc] %%~nxF
  g++ %CXXFLAGS% -o "obj\%%~nF.o" %%F
  if errorlevel 1 exit /b 1
)

echo [ld] linking
g++ -shared -o dist\MOExperience.dll obj\MOExperience.o obj\SyringeHooks.o obj\GdiText.o obj\Lang.o obj\MsgText.o obj\SWPanel.o obj\PowerDisplay.o obj\VecText.o obj\TypeSelect.o obj\AirWay.o obj\BuildQueue.o obj\PatrolCircle.o obj\RallyPath.o obj\SmartOrder.o obj\TreeChop.o obj\FuzzyAttack.o obj\AttackMove.o obj\SmartArea.o obj\OrderUtil.o obj\MindSelect.o obj\Observer.o obj\DeployPreview.o obj\WeaponRange.o obj\ToolTip.o obj\ChatEdit.o obj\FullMap.o obj\ScaleBar.o obj\PauseView.o obj\Shot.o obj\FrameCap.o obj\GameSpeed.o obj\PiP.o obj\GridView.o obj\SideScroll.o obj\SWSBar.o obj\SWSTest.o obj\RepairAll.o obj\PlaceScatter.o obj\PlacementPreview.o obj\OrderThrottle.o obj\WaypointCap.o obj\MegaListCap.o obj\NoApproach.o obj\PvpNotice.o obj\Aggressive.o obj\EscMenu.o obj\PlayerColor.o obj\SidebarSkin.o obj\Icon.o obj\PcxImage.o obj\icons.res.o ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -limm32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
if errorlevel 1 exit /b 1

copy /Y dist\MOExperience.dll ..\MOExperience.dll >nul
if errorlevel 1 exit /b 1
rem Hooks are embedded in .syhks00 now (no .inj needed, like AutoLoadDev).
rem A stale game-dir .inj would declare every hook twice, so remove it.
if exist ..\MOExperience.dll.inj del ..\MOExperience.dll.inj >nul
echo [full] ok

