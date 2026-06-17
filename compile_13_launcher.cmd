@echo off
setlocal
set "ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\compile_13_launcher.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" (
    echo 13_Launcher compile failed with exit code %EXIT_CODE%.
) else (
    echo 13_Launcher compile finished successfully.
)
exit /b %EXIT_CODE%
