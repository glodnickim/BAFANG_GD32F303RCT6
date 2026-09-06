@echo off
setlocal
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo ERROR: Python is not available in PATH.
  exit /b 2
)
python tools\verify_all.py --require-target
if errorlevel 1 (
  echo.
  echo EVistDrive verification/build FAILED.
  exit /b 1
)
echo.
echo EVistDrive verification and exact target build PASSED.
exit /b 0
