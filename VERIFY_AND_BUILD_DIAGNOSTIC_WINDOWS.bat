@echo off
setlocal
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo ERROR: Python is not available in PATH.
  exit /b 2
)
python tools\verify_all.py --require-target --target-variant diagnostic
if errorlevel 1 (
  echo.
  echo EVistDrive diagnostic verification/build FAILED.
  exit /b 1
)
echo.
echo EVistDrive DIAGNOSTIC build with FW145 live telemetry PASSED.
echo Use the generated *_M820_BL820_DIAG.bin for CANable Level-4 ride logging.
exit /b 0
