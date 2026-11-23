@echo off
rem Builds mod\version.dll (x64, static CRT). Requires Visual Studio C++ tools.
setlocal
pushd "%~dp0"
set "PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail
if not exist build mkdir build
set MH=third_party\minhook\src
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DUNICODE /D_UNICODE /Ithird_party\minhook\include ^
   /Fobuild\ /LD src\dmsubs.cpp src\overlay.cpp src\subtitles.cpp src\unity.cpp src\capture.cpp src\symbols.cpp ^
   %MH%\hook.c %MH%\buffer.c %MH%\trampoline.c %MH%\hde\hde64.c ^
   /link /OUT:mod\version.dll /IMPLIB:build\version.lib user32.lib gdi32.lib || goto :fail
popd
echo OK: mod\version.dll
exit /b 0
:fail
popd
echo BUILD FAILED
exit /b 1
