@echo off
setlocal
cd /d "%~dp0"
where powershell.exe >nul 2>&1
if errorlevel 1 (
    echo Windows PowerShell was not found.
    pause
    exit /b 1
)
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Publish-GitHub.ps1" %*
set "result=%ERRORLEVEL%"
echo.
if "%result%"=="0" (
    echo Script finished. Check the GitHub links above.
) else (
    echo Publication failed. Review the message above.
)
pause
exit /b %result%
