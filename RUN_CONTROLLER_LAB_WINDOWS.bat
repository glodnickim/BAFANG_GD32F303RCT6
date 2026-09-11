@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem --- Resolve a host C compiler -------------------------------------------------
rem Controller Lab builds the production control path with a host gcc. Same candidate
rem fallback style as tools\verify_all.py uses for the Arm target toolchain.
if not defined CC set "CC=gcc"
where %CC% >nul 2>nul
if errorlevel 1 (
  if exist "C:\Projekty\tools\w64devkit\bin\gcc.exe" (
    set "PATH=C:\Projekty\tools\w64devkit\bin;%PATH%"
    set "CC=gcc"
  )
)
where %CC% >nul 2>nul
if errorlevel 1 (
  echo ERROR: no host C compiler found.
  echo Put gcc in PATH or set CC to a host compiler, then re-run.
  pause
  exit /b 2
)

rem --- Resolve Python ------------------------------------------------------------
set "LABPY="
where python >nul 2>nul && set "LABPY=python"
if not defined LABPY (
  where py >nul 2>nul && set "LABPY=py"
)
if not defined LABPY (
  for %%D in ("%LOCALAPPDATA%\Programs\Python\Python313" "%LOCALAPPDATA%\Programs\Python\Python312" "%LOCALAPPDATA%\Programs\Python\Python311") do (
    if exist "%%~D\python.exe" if not defined LABPY set "LABPY=%%~D\python.exe"
  )
)
if not defined LABPY (
  echo ERROR: Python 3 is not available in PATH.
  pause
  exit /b 2
)

"%LABPY%" sim\controller_lab\server.py --open
if errorlevel 1 pause
endlocal
