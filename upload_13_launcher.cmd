@echo off
setlocal
set "ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\upload_13_launcher.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" (
    echo 13_Launcher upload failed with exit code %EXIT_CODE%.
) else (
    echo 13_Launcher upload finished successfully.
)
exit /b %EXIT_CODE%
