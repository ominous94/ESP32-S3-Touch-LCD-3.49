@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
python -m PyInstaller --onefile --windowed --name CodexStatusService --add-data "tools;tools" tools\codex_status_gui.py
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" (
  echo.
  echo Build failed. Install dependencies with:
  echo   python -m pip install PySide6 pyinstaller
  exit /b %EXIT_CODE%
)
echo.
echo Built: %ROOT%dist\CodexStatusService.exe
exit /b 0
