@echo off
rem Builds rstrtmgr.dll (RepairReloadFix) with the MSVC x64 toolset (Visual Studio 2022 or Build Tools 2022).
rem The same file also works when renamed to dinput8.dll.
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo MSVC x64 toolset not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 || exit /b 1
if not exist build mkdir build
ml64 /nologo /c /W3 /WX /Fobuild\RmStubs.obj src\RmStubs.asm || exit /b 1
cl /nologo /LD /O2 /MT /EHsc /W4 /WX /permissive- /std:c++17 /Fobuild\ src\RepairReloadFix.cpp build\RmStubs.obj /link /NOLOGO /DEF:src\proxy.def /OUT:build\rstrtmgr.dll /IMPLIB:build\rstrtmgr.lib || exit /b 1
echo built: build\rstrtmgr.dll
