@echo off
setlocal
if /i "%VSCMD_ARG_TGT_ARCH%"=="x86" goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer vswhere.exe was not found. Install the Desktop development with C++ workload. 1>&2
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo No Visual Studio installation with x86 C++ tools was found. 1>&2
    exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
if errorlevel 1 exit /b 1
:build
pushd "%~dp0"
if not exist build mkdir build
if not exist build\detours mkdir build\detours
rem Microsoft Detours v4.0.1, commit e4bfd6b03e50de46b47abfbd1e46b384f0c5f833.
cl /nologo /W4 /WX /O2 /MT /Gy /EHsc /DWIN32_LEAN_AND_MEAN /c vendor\detours\detours.cpp vendor\detours\modules.cpp vendor\detours\disasm.cpp vendor\detours\image.cpp vendor\detours\creatwth.cpp vendor\detours\disolx86.cpp vendor\detours\disolx64.cpp vendor\detours\disolia64.cpp vendor\detours\disolarm.cpp vendor\detours\disolarm64.cpp /Fobuild\detours\
if errorlevel 1 goto failed
lib /nologo /OUT:build\detours.lib /MACHINE:X86 build\detours\*.obj
if errorlevel 1 goto failed
lib /nologo /DEF:stock_openvr.def /OUT:build\stock_openvr.lib /MACHINE:X86
if errorlevel 1 goto failed
cl /nologo /std:c++17 /W4 /WX /O2 /MT /EHsc /DUNICODE /D_UNICODE /external:Ivendor /external:Ivendor\detours /external:W0 /LD openvr_hook.cpp build\stock_openvr.lib build\detours.lib /Fobuild\openvr_hook.obj /link /DEF:openvr_hook.def /OUT:build\ccd_vr_hook.dll /IMPLIB:build\ccd_vr_hook.lib /MACHINE:X86 /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /Brepro
if errorlevel 1 goto failed
cl /nologo /std:c++17 /W4 /WX /O2 /MT /EHsc /DUNICODE /D_UNICODE /external:Ivendor /external:Ivendor\detours /external:W0 launcher.cpp build\detours.lib /Fobuild\launcher.obj /Febuild\ccd_vr_launcher.exe /link /MACHINE:X86 /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /Brepro
if errorlevel 1 goto failed
if not exist build\ccd_vr_projection.ini copy /y ccd_vr_projection.ini build\ccd_vr_projection.ini >nul
if errorlevel 1 goto failed
popd
exit /b 0
:failed
set "BUILD_EXIT=%errorlevel%"
popd
exit /b %BUILD_EXIT%
