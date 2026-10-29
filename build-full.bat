@echo off
rem ============================================================================
rem  build-full.bat  -  rebuild everything: wipe obj\ then run the normal
rem  incremental build. A missing .o always counts as stale, so every .cpp is
rem  compiled (and icons.res.o too). Usage: first build after a clone, after
rem  changing compiler flags, or when the incremental result looks wrong.
rem  Link/deploy steps are not duplicated here; they live in build-fast.bat.
rem ============================================================================
call "%~dp0build-fast.bat" /clean
exit /b %errorlevel%
