@echo off
rem Copies the build to %LOCALAPPDATA%\wcap, makes the tray app start with Windows, adds a Start menu
rem shortcut and points WCAP_CLI at the installed wcap-cli. Run it after build.cmd.
setlocal enabledelayedexpansion
cd /d "%~dp0"

if "%PROCESSOR_ARCHITECTURE%" equ "ARM64" (set ARCH=arm64) else (set ARCH=x64)
if "%~1" neq "" set ARCH=%~1
set "DEST=%LOCALAPPDATA%\wcap"
set "APP=wcap-%ARCH%.exe"
set "CLI=wcap-cli-%ARCH%.exe"

if not exist "%APP%" (
  echo ERROR: %APP% not found, run build.cmd first
  exit /b 1
)
if not exist "%CLI%" (
  echo ERROR: %CLI% not found, run build.cmd first
  exit /b 1
)
if not exist "%DEST%" mkdir "%DEST%"

rem the tray app locks its exe while running: close it (from wherever it runs), copy, start it again
set RUNNING=0
tasklist /FI "IMAGENAME eq %APP%" 2>nul | "%SystemRoot%\System32\find.exe" /I "%APP%" >nul && set RUNNING=1
taskkill /IM "%APP%" /F >nul 2>&1
ping -n 2 127.0.0.1 >nul

copy /Y "%APP%" "%DEST%\" >nul || exit /b 1
copy /Y "%CLI%" "%DEST%\" >nul || exit /b 1

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v wcap /t REG_SZ /d "\"%DEST%\%APP%\"" /f >nul || exit /b 1
setx WCAP_CLI "%DEST%\%CLI%" >nul || exit /b 1
powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Programs')+'\wcap.lnk'); $s.TargetPath='%DEST%\%APP%'; $s.WorkingDirectory='%DEST%'; $s.Description='wcap'; $s.Save()" || exit /b 1

if "%RUNNING%"=="1" start "" "%DEST%\%APP%"

echo Installed in %DEST%
echo Settings are read from %DEST%\wcap-%ARCH%.ini and %DEST%\wcap-cli-%ARCH%.ini
