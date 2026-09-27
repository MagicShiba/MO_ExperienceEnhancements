set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
g++ -O2 -Wall -fno-exceptions -fno-rtti -shared -o dist/MOExperience.dll src/MOExperience.cpp src/GdiText.cpp src/VecText.cpp src/TypeSelect.cpp src/AirWay.cpp src/BuildQueue.cpp src/PatrolCircle.cpp src/SmartOrder.cpp src/TreeChop.cpp src/FuzzyAttack.cpp src/SmartArea.cpp src/OrderUtil.cpp src/MindSelect.cpp ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
if errorlevel 1 exit /b 1
copy /Y dist\MOExperience.dll ..\MOExperience.dll >nul
if errorlevel 1 exit /b 1
copy /Y dist\MOExperience.dll.inj ..\MOExperience.dll.inj >nul
if errorlevel 1 exit /b 1
