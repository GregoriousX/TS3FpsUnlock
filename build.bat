@echo off
setlocal
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /O2 /MT /W4 /EHsc /LD src\dllmain.cpp /Fo:bin\ /Fe:bin\TS3FpsUnlock.asi /link kernel32.lib winmm.lib || exit /b 1
copy /y TS3FpsUnlock.ini bin\ >nul
echo Built bin\TS3FpsUnlock.asi
