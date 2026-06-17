@echo off
setlocal
set "ROOT=%~dp0"
python "%ROOT%tools\verify_13_launcher.py" %*
set "EXIT_CODE=%ERRORLEVEL%"
echo.
if not "%EXIT_CODE%"=="0" (
    echo 13_Launcher verification failed with exit code %EXIT_CODE%.
) else (
    echo 13_Launcher verification finished successfully.
)
echo Press any key to close this window.
pause >nul
exit /b %EXIT_CODE%
