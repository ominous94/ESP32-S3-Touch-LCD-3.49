@echo off
setlocal
set "ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\compile_codex_status.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" (
    echo Codex Status compile failed with exit code %EXIT_CODE%.
) else (
    echo Codex Status compile finished successfully.
)
echo Press any key to close this window.
pause >nul
exit /b %EXIT_CODE%
