set PATH=G:\motest\AutoLoadDev\tools\mingw\mingw32\bin;%PATH%
rem 探针阶段已结束, 平时不编; 需要时手动跑本脚本
g++ -O2 -Wall -fno-exceptions -fno-rtti -shared -o dist/Probe.dll src/Probe.cpp ^
  -nostdlib -lkernel32 -lgcc ^
  -Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -Wl,--entry,_DllMain@12
