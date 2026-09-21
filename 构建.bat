set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
g++ -O2 -Wall -fno-exceptions -fno-rtti -shared -o dist/MsgFont.dll src/MsgFont.cpp src/GdiText.cpp src/VecText.cpp ^
  -nostdlib -lkernel32 -luser32 -lgdi32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
