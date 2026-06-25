@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~1"
if "%PROJECT_ROOT%"=="" set "PROJECT_ROOT=%~dp0."

set "SCRIPT=%USERPROFILE%\.varka\build-clean-scripts\ESP-IDF_Clean.ps1"
pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -ProjectRoot "%PROJECT_ROOT%"
set "code=%ERRORLEVEL%"
if not "%code%"=="0" (
    echo.
    echo Cleanup failed. This window is kept open for the error.
    pause
)
exit /b %code%