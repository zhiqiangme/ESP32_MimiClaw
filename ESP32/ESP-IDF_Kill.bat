@echo off
setlocal EnableExtensions

rem ESP-IDF generated-file cleaner.
rem Keep source code, sdkconfig, components and dependency lock files intact.

cd /d "%~dp0"

echo.
echo [Clean] Project: %CD%
echo.

call :CheckEspIdfProject || goto :ErrorExit

call :RemoveDir "build" || goto :ErrorExit
call :RemoveDir ".cache" || goto :ErrorExit
call :RemoveDir "CMakeFiles" || goto :ErrorExit

call :RemoveFile "CMakeCache.txt" || goto :ErrorExit
call :RemoveFile "cmake_install.cmake" || goto :ErrorExit
call :RemoveFile "compile_commands.json" || goto :ErrorExit
call :RemoveFile "project_description.json" || goto :ErrorExit
call :RemoveFile "flasher_args.json" || goto :ErrorExit
call :RemoveFile "flash_args" || goto :ErrorExit
call :RemoveFile "sdkconfig.old" || goto :ErrorExit

echo.
echo [Clean] Done.
exit /b 0

:CheckEspIdfProject
set "HAS_IDF_CMAKE="
set "HAS_APP_STRUCTURE="

if not exist "CMakeLists.txt" (
    echo [Error] CMakeLists.txt was not found.
    echo [Error] Please run this script from an ESP-IDF project root.
    exit /b 1
)

findstr /C:"project.cmake" "CMakeLists.txt" >nul 2>nul
if %ERRORLEVEL%==0 set "HAS_IDF_CMAKE=1"

if not defined HAS_IDF_CMAKE (
    echo [Error] CMakeLists.txt does not include ESP-IDF project.cmake.
    echo [Error] This does not look like an ESP-IDF project root.
    exit /b 1
)

if exist "main\CMakeLists.txt" set "HAS_APP_STRUCTURE=1"
if exist "components\" set "HAS_APP_STRUCTURE=1"

if not defined HAS_APP_STRUCTURE (
    echo [Error] main\CMakeLists.txt or components directory was not found.
    echo [Error] This does not look like an ESP-IDF app project.
    exit /b 1
)

exit /b 0

:RemoveDir
if exist "%~1\" (
    echo [Delete dir] %~1
    rmdir /s /q "%~1"
    if exist "%~1\" (
        echo [Error] Failed to delete directory: %~1
        exit /b 1
    )
)
exit /b 0

:RemoveFile
if exist "%~1" (
    echo [Delete file] %~1
    del /f /q "%~1"
    if exist "%~1" (
        echo [Error] Failed to delete file: %~1
        exit /b 1
    )
)
exit /b 0

:ErrorExit
echo.
echo [Clean] Failed. Check the error above.
pause
exit /b 1
