@echo off
setlocal
if not "%~1"=="" (
    echo This launcher accepts no options. It starts this CCD installation only. 1>&2
    exit /b 1
)
if not exist "%~dp0build\ccd_vr_launcher.exe" (
    echo Missing "%~dp0build\ccd_vr_launcher.exe". Run build.cmd first. 1>&2
    exit /b 1
)
for %%I in ("%~dp0..\bin\win32\Starter.exe") do "%~dp0build\ccd_vr_launcher.exe" "%%~fI"
exit /b %errorlevel%
