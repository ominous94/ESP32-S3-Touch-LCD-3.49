@echo off
setlocal
set "ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\start_codex_status.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" (
    echo Codex Status startup failed with exit code %EXIT_CODE%.
) else (
    echo Codex Status startup command finished. Services keep running in the background.
)
echo Press any key to close this window.
pause >nul
exit /b %EXIT_CODE%
