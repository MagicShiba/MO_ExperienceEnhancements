set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
g++ -O2 -Wall -fno-exceptions -fno-rtti -shared -o dist/MsgFont.dll src/MsgFont.cpp src/GdiText.cpp src/VecText.cpp src/TypeSelect.cpp src/AirWay.cpp src/BuildQueue.cpp src/PatrolCircle.cpp src/AirSpread.cpp src/TreeChop.cpp src/FuzzyAttack.cpp src/SmartArea.cpp src/OrderUtil.cpp src/MindSelect.cpp ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
if errorlevel 1 exit /b 1
copy /Y dist\MsgFont.dll ..\MsgFont.dll >nul
if errorlevel 1 exit /b 1
copy /Y dist\MsgFont.dll.inj ..\MsgFont.dll.inj >nul
if errorlevel 1 exit /b 1
