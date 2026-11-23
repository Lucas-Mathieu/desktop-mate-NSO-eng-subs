@echo off
rem Builds mod\version.dll (x64, static CRT). Needs Visual Studio (any edition, 2019 or newer) or
rem the Build Tools with the "Desktop development with C++" workload. Run from any prompt.
setlocal
pushd "%~dp0"
where cl >nul 2>nul && goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto :novs
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail
:build
if not exist build mkdir build
set MH=third_party\minhook\src
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DUNICODE /D_UNICODE /Ithird_party\minhook\include ^
   /Fobuild\ /LD src\dmsubs.cpp src\overlay.cpp src\subtitles.cpp src\unity.cpp src\capture.cpp src\symbols.cpp ^
   %MH%\hook.c %MH%\buffer.c %MH%\trampoline.c %MH%\hde\hde64.c ^
   /link /OUT:mod\version.dll /IMPLIB:build\version.lib user32.lib gdi32.lib || goto :fail
popd
echo OK: mod\version.dll
exit /b 0
:novs
echo Visual Studio C++ tools not found - install "Desktop development with C++" (Visual Studio or Build Tools)
:fail
popd
echo BUILD FAILED
exit /b 1
